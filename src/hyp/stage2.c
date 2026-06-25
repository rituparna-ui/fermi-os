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

static uint64_t s2_leaf_flags(int device) {
  uint64_t f = PTE_VALID | PTE_AF | S2_S2AP_RW;
  if (device) {
    f |= S2_MEMATTR_DEVICE_nGnRE | S2_XN_ALL; /* SH ignored for Device */
  } else {
    f |= S2_MEMATTR_NORMAL_WB | PTE_SH_INNER | S2_XN_NONE;
  }
  return f;
}

/* Allocate a fresh concatenated 1024-entry L1 root (8 KiB, 8 KiB aligned). */
static uint64_t *s2_alloc_l1(void) {
  return (uint64_t *)hyp_alloc_aligned(2, 0x2000);
}

/* Return the L2 table for the 1 GiB region containing `ipa` in table `l1`,
 * allocating it if absent. */
static uint64_t *s2_get_l2(uint64_t *l1, uint64_t ipa) {
  uint64_t i = S2_L1_INDEX(ipa);
  uint64_t e = l1[i];
  if (!(e & PTE_VALID)) {
    uint64_t *l2 = (uint64_t *)hyp_alloc_pages(1);
    l1[i] = ((uint64_t)(uintptr_t)l2 & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    return l2;
  }
  if (!(e & PTE_TABLE)) {
    hyp_panic("s2_get_l2: L1 entry is a 1GiB block, cannot split");
  }
  return (uint64_t *)(uintptr_t)(e & PTE_ADDR_MASK);
}

static uint64_t *s2_get_l3(uint64_t *l1, uint64_t ipa) {
  uint64_t *l2 = s2_get_l2(l1, ipa);
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

void s2_map_range_in(uint64_t *l1, uint64_t ipa, uint64_t pa, uint64_t size,
                     int device) {
  uint64_t leaf = s2_leaf_flags(device);
  uint64_t end = ipa + size;

  while (ipa < end) {
    uint64_t remain = end - ipa;

    if ((ipa % S2_1GB) == 0 && (pa % S2_1GB) == 0 && remain >= S2_1GB) {
      l1[S2_L1_INDEX(ipa)] = (pa & PTE_ADDR_MASK) | leaf | PTE_BLOCK;
      ipa += S2_1GB;
      pa += S2_1GB;
      continue;
    }
    if ((ipa % S2_2MB) == 0 && (pa % S2_2MB) == 0 && remain >= S2_2MB) {
      uint64_t *l2 = s2_get_l2(l1, ipa);
      l2[S2_L2_INDEX(ipa)] = (pa & PTE_ADDR_MASK) | leaf | PTE_BLOCK;
      ipa += S2_2MB;
      pa += S2_2MB;
      continue;
    }
    uint64_t *l3 = s2_get_l3(l1, ipa);
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

void s2_init_vtcr(void) {
  /* One-time: program VTCR_EL2 (shared IPA geometry for every VM). */
  uint64_t vtcr = 0x80023558ULL; /* T0SZ=24,SL0=1,WBWA,IS,4K,PS=40b,VS=8b,RES1 */
  __asm__ __volatile__("msr vtcr_el2, %0\n\tisb" ::"r"(vtcr));
  hyp_puts("[S2] VTCR_EL2=");
  hyp_puthex(vtcr);
  hyp_putc('\n');
}

uint64_t s2_make_vttbr(uint64_t l1_root, uint32_t vmid) {
  return (l1_root & ~0x1FFFULL) | ((uint64_t)vmid << 48);
}

/* Clean all hyp pool memory to PoC so the WB-cacheable stage-2 walker observes
 * the descriptors written with EL2 caches off. Called once after building both
 * VMs' tables (the bump allocator is contiguous). */
static void s2_clean_tables(void) {
  /* Use a fixed low base (the first stage-2 table allocated). The hyp pool is
   * contiguous from just above __hyp_end; cleaning from HYP_PHYS_BASE upward
   * covers every table page. */
  extern uint8_t __hyp_end[];
  uint64_t base = (uint64_t)(uintptr_t)__hyp_end;
  hyp_dcache_clean_range(base, HYP_RAM_TOP - base);
}

uint64_t s2_build_vm1(void) {
  hyp_puts("[S2] building VM1 (FermiOS) stage-2 identity map (IPA==PA)\n");
  uint64_t *l1 = s2_alloc_l1();

  /* Guest RAM 0x40000000..0x240000000 (8 GiB) — eight 1 GiB Normal-WB blocks. */
  s2_map_range_in(l1, 0x40000000ULL, 0x40000000ULL, 0x200000000ULL, 0);

  /* PL011 UART (single 4 KiB Device page). */
  s2_map_range_in(l1, 0x09000000ULL, 0x09000000ULL, S2_PAGE, 1);

  /* GICD/GICR LEFT INVALID -> guest MMIO traps to the vGIC model. */

  /* PCI MMIO32 [0x10000000, 0x3F000000) Device. */
  s2_map_range_in(l1, 0x10000000ULL, 0x10000000ULL, 0x2F000000ULL, 1);
  /* PCI ECAM (high) 256 MiB Device. */
  s2_map_range_in(l1, 0x4010000000ULL, 0x4010000000ULL, 0x10000000ULL, 1);
  /* PCI MMIO64 512 GiB Device. */
  s2_map_range_in(l1, 0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL, 1);

  s2_clean_tables();
  return (uint64_t)(uintptr_t)l1;
}

uint64_t s2_build_vm2(uint64_t host_ram_base, uint64_t ram_size) {
  hyp_puts("[S2] building VM2 stage-2: IPA 0x40000000 -> host PA ");
  hyp_puthex(host_ram_base);
  hyp_putc('\n');
  uint64_t *l1 = s2_alloc_l1();

  /* VM2 sees RAM at the SAME IPA 0x40000000 as VM1, but it maps to a DIFFERENT
   * host PA — the isolation demonstration. ram_size rounded to 2 MiB. */
  s2_map_range_in(l1, 0x40000000ULL, host_ram_base, ram_size, 0);

  /* VM2 prints to the same UART (Device, straight-through IPA==PA). */
  s2_map_range_in(l1, 0x09000000ULL, 0x09000000ULL, S2_PAGE, 1);

  s2_clean_tables();
  return (uint64_t)(uintptr_t)l1;
}
