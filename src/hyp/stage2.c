#include "stage2.h"
#include "mm/pmm/pmm.h"
#include "mm/mmu/mmu.h" /* PHYS_TO_VIRT, VIRT_TO_PHYS */
#include "uart/uart.h"

/* Stage-2 table builder for the VHE host (stage-1 MMU on).
 *
 * Table pages come from the PMM (physical) and are edited via PHYS_TO_VIRT.
 * The hardware stage-2 walker reads the PHYSICAL descriptors, so every stored
 * table pointer is a physical address masked into the descriptor. */

/* Allocate one zeroed 4 KiB table page; return its PHYSICAL address. */
static uint64_t s2_alloc_table(void) {
  uintptr_t pa = pmm_allocate_page();
  if (!pa) {
    return 0;
  }
  uint64_t *v = (uint64_t *)PHYS_TO_VIRT(pa);
  for (int i = 0; i < 512; i++) {
    v[i] = 0;
  }
  return (uint64_t)pa;
}

static uint64_t s2_leaf_flags(int device) {
  uint64_t f = S2_PTE_VALID | S2_PTE_AF | S2_S2AP_RW;
  if (device) {
    f |= S2_MEMATTR_DEVICE_nGnRE | S2_XN_ALL; /* SH ignored for Device */
  } else {
    f |= S2_MEMATTR_NORMAL_WB | S2_PTE_SH_IS | S2_XN_NONE;
  }
  return f;
}

/* Return the VIRT pointer to the L2 table for `ipa`, allocating if needed. */
static uint64_t *s2_get_l2(stage2_t *s2, uint64_t ipa) {
  uint64_t i = S2_L1_INDEX(ipa);
  uint64_t e = s2->l1_virt[i];
  if (!(e & S2_PTE_VALID)) {
    uint64_t l2_phys = s2_alloc_table();
    s2->l1_virt[i] = (l2_phys & S2_ADDR_MASK) | S2_PTE_VALID | S2_PTE_TABLE;
    return (uint64_t *)PHYS_TO_VIRT(l2_phys);
  }
  return (uint64_t *)PHYS_TO_VIRT(e & S2_ADDR_MASK);
}

static uint64_t *s2_get_l3(stage2_t *s2, uint64_t ipa) {
  uint64_t *l2 = s2_get_l2(s2, ipa);
  uint64_t i = S2_L2_INDEX(ipa);
  uint64_t e = l2[i];
  if (!(e & S2_PTE_VALID)) {
    uint64_t l3_phys = s2_alloc_table();
    l2[i] = (l3_phys & S2_ADDR_MASK) | S2_PTE_VALID | S2_PTE_TABLE;
    return (uint64_t *)PHYS_TO_VIRT(l3_phys);
  }
  return (uint64_t *)PHYS_TO_VIRT(e & S2_ADDR_MASK);
}

void stage2_map(stage2_t *s2, uint64_t ipa, uint64_t pa, uint64_t size,
                int device) {
  uint64_t leaf = s2_leaf_flags(device);
  uint64_t end = ipa + size;

  while (ipa < end) {
    uint64_t remain = end - ipa;

    /* 1 GiB L1 block. */
    if ((ipa % S2_1GB) == 0 && (pa % S2_1GB) == 0 && remain >= S2_1GB) {
      s2->l1_virt[S2_L1_INDEX(ipa)] = (pa & S2_ADDR_MASK) | leaf | S2_PTE_BLOCK;
      ipa += S2_1GB;
      pa += S2_1GB;
      continue;
    }

    /* 2 MiB L2 block. */
    if ((ipa % S2_2MB) == 0 && (pa % S2_2MB) == 0 && remain >= S2_2MB) {
      uint64_t *l2 = s2_get_l2(s2, ipa);
      l2[S2_L2_INDEX(ipa)] = (pa & S2_ADDR_MASK) | leaf | S2_PTE_BLOCK;
      ipa += S2_2MB;
      pa += S2_2MB;
      continue;
    }

    /* 4 KiB L3 page (leaf uses the table/page bit, like stage-1 L3). */
    uint64_t *l3 = s2_get_l3(s2, ipa);
    l3[S2_L3_INDEX(ipa)] = (pa & S2_ADDR_MASK) | leaf | S2_PTE_TABLE;
    ipa += S2_PAGE;
    pa += S2_PAGE;
  }

  /* Publish the descriptor writes to the (cacheable, IS) stage-2 walker. */
  __asm__ __volatile__("dsb ish" ::: "memory");
}

int stage2_create(stage2_t *s2, uint32_t vmid) {
  /* Concatenated 1024-entry L1 = two contiguous 4 KiB pages, 8 KiB aligned so
   * VTTBR_EL2.BADDR alignment holds. pmm_allocate_pages is page-granular; ask
   * for 2 and rely on the bitmap allocator's natural alignment, then assert. */
  uintptr_t root = pmm_allocate_pages(2);
  if (!root || (root & 0x1FFF)) {
    /* Need 8 KiB alignment; if the 2-page run isn't aligned, grab 3 and use the
     * aligned 8 KiB window inside it. */
    if (root) {
      pmm_free_pages(root, 2);
    }
    uintptr_t wide = pmm_allocate_pages(3);
    if (!wide) {
      uart_errorln("[S2] failed to allocate L1 root");
      return 0;
    }
    root = (wide + 0x1FFF) & ~0x1FFFULL;
  }

  uint64_t *v = (uint64_t *)PHYS_TO_VIRT(root);
  for (int i = 0; i < 1024; i++) {
    v[i] = 0;
  }

  s2->l1_virt = v;
  s2->root_phys = (uint64_t)root;
  s2->vmid = vmid;
  __asm__ __volatile__("dsb ish" ::: "memory");
  return 1;
}

uint64_t stage2_vttbr(const stage2_t *s2) {
  return (s2->root_phys & ~0x1FFFULL) | ((uint64_t)s2->vmid << 48);
}

uint64_t stage2_translate(const stage2_t *s2, uint64_t ipa) {
  /* L1: concatenated 1024-entry table (1 GiB per entry). */
  uint64_t e = s2->l1_virt[S2_L1_INDEX(ipa)];
  if (!(e & S2_PTE_VALID)) {
    return 0;
  }
  if (!(e & S2_PTE_TABLE)) {
    /* 1 GiB block leaf. */
    return (e & S2_ADDR_MASK & ~(S2_1GB - 1)) | (ipa & (S2_1GB - 1));
  }

  /* L2: 512 entries (2 MiB per entry). */
  uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(e & S2_ADDR_MASK);
  e = l2[S2_L2_INDEX(ipa)];
  if (!(e & S2_PTE_VALID)) {
    return 0;
  }
  if (!(e & S2_PTE_TABLE)) {
    /* 2 MiB block leaf. */
    return (e & S2_ADDR_MASK & ~(S2_2MB - 1)) | (ipa & (S2_2MB - 1));
  }

  /* L3: 512 entries (4 KiB pages). A valid L3 leaf has the table/page bit set. */
  uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(e & S2_ADDR_MASK);
  e = l3[S2_L3_INDEX(ipa)];
  if (!(e & S2_PTE_VALID)) {
    return 0;
  }
  return (e & S2_ADDR_MASK) | (ipa & (S2_PAGE - 1));
}
