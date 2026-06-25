#include "stage2.h"
#include "hyp.h"
#include "hyp_alloc.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Stage-2 identity translation.
 *
 * The hypervisor runs at EL2 with its OWN stage-1 MMU off, so every pointer
 * here is a physical address and table pages are dereferenced directly. The
 * stage-2 walker (hardware) reads these tables as Normal WB-cacheable per
 * VTCR_EL2.IRGN0/ORGN0, so we clean them to PoC after building.
 *
 * Layout for 40-bit IPA, 4 KiB granule, SL0=1:
 *   L1: concatenated 1024-entry table (IPA[39:30]) — two contiguous 4 KiB
 *       pages, 8 KiB aligned. Each entry maps 1 GiB (block) or points at an L2.
 *   L2: 512 entries (IPA[29:21]), 2 MiB blocks or L3 tables.
 *   L3: 512 entries (IPA[20:12]), 4 KiB pages.
 * ------------------------------------------------------------------------- */

#define S2_L1_ENTRIES 1024

static uint64_t *s2_l1_root; /* host PA of the concatenated L1 (== VA, MMU off) */

static uint64_t s2_leaf_flags(int device) {
  uint64_t f = PTE_VALID | PTE_AF | S2_S2AP_RW;
  if (device) {
    f |= S2_MEMATTR_DEVICE_nGnRE | S2_XN_ALL; /* SH ignored for Device */
  } else {
    f |= S2_MEMATTR_NORMAL_WB | PTE_SH_INNER | S2_XN_NONE;
  }
  return f;
}

/* Return the L2 table for the 1 GiB region containing `ipa`, allocating it if
 * the L1 entry is not yet a table. Splits are never needed because we build
 * the map once from scratch. */
static uint64_t *s2_get_l2(uint64_t ipa) {
  uint64_t i = S2_L1_INDEX(ipa);
  uint64_t e = s2_l1_root[i];
  if (!(e & PTE_VALID)) {
    uint64_t *l2 = (uint64_t *)hyp_alloc_pages(1);
    s2_l1_root[i] = ((uint64_t)(uintptr_t)l2 & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    return l2;
  }
  if (!(e & PTE_TABLE)) {
    hyp_panic("s2_get_l2: L1 entry is a 1GiB block, cannot split");
  }
  return (uint64_t *)(uintptr_t)(e & PTE_ADDR_MASK);
}

static uint64_t *s2_get_l3(uint64_t ipa) {
  uint64_t *l2 = s2_get_l2(ipa);
  uint64_t i = S2_L2_INDEX(ipa);
  uint64_t e = l2[i];
  if (!(e & PTE_VALID)) {
    uint64_t *l3 = (uint64_t *)hyp_alloc_pages(1);
    l2[i] = ((uint64_t)(uintptr_t)l3 & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    return l3;
  }
  if (!(e & PTE_TABLE)) {
    hyp_panic("s2_get_l3: L2 entry is a 2MiB block, cannot split");
  }
  return (uint64_t *)(uintptr_t)(e & PTE_ADDR_MASK);
}

void s2_map_range(uint64_t ipa, uint64_t pa, uint64_t size, int device) {
  uint64_t leaf = s2_leaf_flags(device);
  uint64_t end = ipa + size;

  while (ipa < end) {
    uint64_t remain = end - ipa;

    /* 1 GiB L1 block: IPA+PA 1 GiB-aligned and >=1 GiB remaining. */
    if ((ipa % S2_1GB) == 0 && (pa % S2_1GB) == 0 && remain >= S2_1GB) {
      s2_l1_root[S2_L1_INDEX(ipa)] =
          (pa & PTE_ADDR_MASK) | leaf | PTE_BLOCK;
      ipa += S2_1GB;
      pa += S2_1GB;
      continue;
    }

    /* 2 MiB L2 block. */
    if ((ipa % S2_2MB) == 0 && (pa % S2_2MB) == 0 && remain >= S2_2MB) {
      uint64_t *l2 = s2_get_l2(ipa);
      l2[S2_L2_INDEX(ipa)] = (pa & PTE_ADDR_MASK) | leaf | PTE_BLOCK;
      ipa += S2_2MB;
      pa += S2_2MB;
      continue;
    }

    /* 4 KiB L3 page. L3 leaf uses PTE_TABLE (bit[1]=1) like stage-1 pages. */
    uint64_t *l3 = s2_get_l3(ipa);
    l3[S2_L3_INDEX(ipa)] = (pa & PTE_ADDR_MASK) | leaf | PTE_TABLE;
    ipa += S2_PAGE;
    pa += S2_PAGE;
  }
}

void s2_tlb_flush_all(void) {
  /* Invalidate ALL stage-1 + stage-2 TLB entries for the current VMID,
   * inner-shareable. VTTBR_EL2.VMID must already be programmed. */
  __asm__ __volatile__("dsb ish\n\t"
                       "tlbi vmalls12e1is\n\t"
                       "dsb ish\n\t"
                       "isb" ::: "memory");
}

uint64_t s2_init(void) {
  hyp_puts("[S2] building stage-2 identity tables (IPA==PA)\n");

  /* Concatenated 1024-entry L1 = two contiguous 4 KiB pages, 8 KiB aligned
   * so VTTBR_EL2.BADDR alignment is satisfied. */
  s2_l1_root = (uint64_t *)hyp_alloc_aligned(2, 0x2000);

  /* Guest RAM: 0x40000000 .. 0x240000000 (8 GiB) — eight 1 GiB Normal-WB
   * blocks. This is the bulk of the map and costs zero L2/L3 pages. */
  s2_map_range(0x40000000ULL, 0x40000000ULL, 0x200000000ULL, /*device=*/0);

  /* --- Device windows (Phase A: map straight-through so the guest boots
   * exactly as M1; M5 will remove the GIC/ECAM mappings to trap them). --- */

  /* PL011 UART (single 4 KiB page; the surrounding 2 MiB has RTC/fw_cfg/GPIO
   * we deliberately leave invalid). */
  s2_map_range(0x09000000ULL, 0x09000000ULL, S2_PAGE, /*device=*/1);

  /* GICv3 distributor (0x08000000) + redistributor (0x080A0000) are LEFT
   * STAGE-2 INVALID so guest MMIO faults to EL2 (EC=0x24) and is serviced by
   * the vGIC software model (vgic_mmio_emulate). The ITS at 0x08080000 is also
   * left invalid — the guest does not use it. */

  /* PCI MMIO32 window 0x10000000..0x3EFEFFFF. Round the size down/up safely:
   * map [0x10000000, 0x3F000000) = 0x2F000000 (752 MiB). 0x10000000 is 2 MiB
   * aligned and 0x2F000000 is a 2 MiB multiple, so this is whole 2 MiB blocks
   * and never spills into the 0x3F000000 PIO/low-ECAM region. */
  s2_map_range(0x10000000ULL, 0x10000000ULL, 0x2F000000ULL, /*device=*/1);

  /* PCI ECAM (high) 0x4010000000, 256 MiB. (Phase A straight-through; the
   * guest enumerates real devices. M5 may trap this for a vPCI model.) */
  s2_map_range(0x4010000000ULL, 0x4010000000ULL, 0x10000000ULL, /*device=*/1);

  /* PCI MMIO64 window 0x8000000000, 512 GiB — 512 one-GiB Device blocks. */
  s2_map_range(0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL, /*device=*/1);

  /* Clean every table page to PoC so the WB-cacheable stage-2 walker sees the
   * descriptors we wrote with the EL2 caches off. Range = the whole hyp pool
   * from the L1 root upward (bump allocator is contiguous). */
  hyp_dcache_clean_range((uint64_t)(uintptr_t)s2_l1_root,
                         HYP_RAM_TOP - (uint64_t)(uintptr_t)s2_l1_root);

  /* Program VTCR_EL2 then VTTBR_EL2 (VMID in [55:48]). */
  uint64_t vtcr = 0x80023558ULL; /* T0SZ=24,SL0=1,WBWA,IS,4K,PS=40b,VS=8b,RES1 */
  __asm__ __volatile__("msr vtcr_el2, %0\n\tisb" ::"r"(vtcr));

  uint64_t vttbr =
      ((uint64_t)(uintptr_t)s2_l1_root & ~0x1FFFULL) | (HYP_VMID << 48);
  __asm__ __volatile__("msr vttbr_el2, %0\n\tdsb ish\n\tisb" ::"r"(vttbr));

  /* Flush stale stage-1+2 TLB for this VMID before the guest runs. */
  s2_tlb_flush_all();

  hyp_puts("[S2] VTCR_EL2=");
  hyp_puthex(vtcr);
  hyp_puts(" VTTBR_EL2=");
  hyp_puthex(vttbr);
  hyp_putc('\n');

  return (uint64_t)(uintptr_t)s2_l1_root;
}
