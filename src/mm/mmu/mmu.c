#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

static uint64_t *l0_table_lo;
static uint64_t *l0_table_hi;

static uint64_t *alloc_table() {
  uint64_t *table_phys = (uint64_t *)pmm_allocate_page();
  if (!table_phys) {
    uart_errorln("[MMU] Failed to allocate table");
    return 0;
  }
  /* Two phases:
   *   - Pre-MMU (early_init / build_identity_tables): MMU is off, so every
   *     pointer is treated as a physical address. Just memset directly.
   *   - Post-MMU: TTBR0 is per-task and may not identity-map RAM, so
   *     dereferencing the physical pointer can fault. Route the zero
   *     through TTBR1 (PHYS_TO_VIRT), which always maps RAM. */
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
  uint64_t *table_va = (sctlr & 1)
                           ? (uint64_t *)PHYS_TO_VIRT((uintptr_t)table_phys)
                           : table_phys;
  memset(table_va, 0, PAGE_SIZE);
  return table_phys;
}

// Build L0 -> L1 -> L2 page table hierarchy
// maps the first 1TB of physical address space using 2MB blocks
// L0[0]: 0-512GB — RAM, device I/O, PCI ECAM (0x4010000000)
// L0[1]: 512GB-1TB — PCI MMIO64 window (0x8000000000+)

// VA layout:
// [47:39] → L0 (9 bits)
// [38:30] → L1 (9 bits)
// [29:21] → L2 (9 bits)
// [20:12] → L3 (9 bits)
// [11:0]  → offset
// Each level has 512 entries (2^9)
static uint64_t *build_identity_tables(uint64_t **out_l1) {
  uint64_t *l0 = alloc_table();
  if (!l0) {
    return 0;
  }

  uint64_t *l1 = alloc_table();
  if (!l1) {
    return 0;
  }
  // L0[0] -> L1 covers first 512GB (0x0 - 0x7FFFFFFFFF)
  // Contains RAM, device I/O, PCI ECAM
  l0[0] = (uint64_t)l1 | PTE_VALID | PTE_TABLE;

  uint64_t mem_end = MEM_START + MEM_SIZE;
  for (uint64_t l1i = 0; l1i < 512; l1i++) {
    uint64_t *l2 = alloc_table();
    if (!l2) {
      return 0;
    }

    l1[l1i] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;

    for (uint64_t l2i = 0; l2i < 512; l2i++) {
      uint64_t phys_addr = l1i * _1GB + l2i * _2MB;
      if (phys_addr == 0) {
        // null ptr deref should fault
        continue;
      }
      // AttrIdx 0 = Device memory, 1 = Normal memory
      // RAM region is normal, everything else (UART, GIC, PCI ECAM) is device
      uint64_t attr = (phys_addr >= MEM_START && phys_addr < mem_end) ? 1 : 0;

      l2[l2i] = phys_addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
                PTE_AP_RW | PTE_ATTRIDX(attr);
    }
  }

  // L0[1] -> L1 covers 512GB - 1TB (0x8000000000 - 0xFFFFFFFFFF)
  // Contains PCI MMIO64 window — all device memory
  uint64_t *l1_hi = alloc_table();
  if (!l1_hi) {
    return 0;
  }
  l0[1] = (uint64_t)l1_hi | PTE_VALID | PTE_TABLE;

  for (uint64_t l1i = 0; l1i < 512; l1i++) {
    uint64_t *l2 = alloc_table();
    if (!l2) {
      return 0;
    }

    l1_hi[l1i] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;

    for (uint64_t l2i = 0; l2i < 512; l2i++) {
      uint64_t phys_addr = _512GB + l1i * _1GB + l2i * _2MB;

      l2[l2i] = phys_addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
                PTE_AP_RW | PTE_ATTRIDX(0); // all device memory
    }
  }

  if (out_l1) {
    *out_l1 = l1;
  }

  return l0;
}

uint64_t *mmu_init() {
  // reference manual - section D8.3
  uart_println("[MMU] Initializing MMU (48 bit VAS, 4kb granule)");

  uint64_t mair = (0x00 << 0) | // AttrIdx 0 = Device memory
                  (0xff << 8);  // AttrIdx 1 = Normal memory

  __asm__ __volatile__("msr mair_el1, %0" ::"r"(mair));

  // TTBR0 (UserSpace)
  // Identity map first 1TB (RAM + device I/O + PCI ECAM + PCI MMIO64)
  uint64_t *l1_table = 0;
  l0_table_lo = build_identity_tables(&l1_table);
  if (!l0_table_lo) {
    uart_errorln("[MMU] Failed to build TTBR0 tables");
    return 0;
  }
  uart_println("[MMU] TTBR0 lower half tables build");

  // TTBR1 (KernelSpace)
  // Map VA 0xFFFF_0000_0000_0000+ -> PA 0x0000+
  // Hardware strips off the upper bits for TTBR1 lookups automatically
  l0_table_hi = build_identity_tables(0);
  if (!l0_table_hi) {
    uart_errorln("[MMU] Failed to build TTBR1 tables");
    return 0;
  }
  uart_println("[MMU] TTBR1 upper half tables build");

  // https://developer.arm.com/documentation/100095/0002/system-control/aarch64-register-descriptions/translation-control-register--el1
  uint64_t tcr =
      // TTBR0
      (16ULL << 0) |    // T0SZ = 16 → 48-bit VA
      (0b01ULL << 8) |  // IRGN0 = Write-Back, Write-Allocate
      (0b01ULL << 10) | // ORGN0 = Write-Back, Write-Allocate
      (0b11ULL << 12) | // SH0 = inner shareable
      (0b00ULL << 14) | // TG0 = 4KB granule
      // TTBR1
      (16ULL << 16) |   // T1SZ = 16 → 48-bit VA
      (0b01ULL << 24) | // IRGN1 = Write-Back, Write-Allocate
      (0b01ULL << 26) | // ORGN1 = Write-Back, Write-Allocate
      (0b11ULL << 28) | // SH1 = inner shareable
      (0b10ULL << 30) | // TG1 = 4KB granule
                        // TG1 and TG0 have different encodings
                        // TG1[31:30] - bit[31]=RES1, bit[30]=0
      // Common
      (0b010ULL << 32) | // IPS = 40-bit PA (1TB) (needed for >4GB RAM)
      // ASID configuration
      (1ULL << 36);      // AS = 1 → 16-bit ASIDs (65536 contexts)
                         // A1 = 0 → ASID source is TTBR0_EL1[63:48]
  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(tcr));
  // ARM ARM (DDI 0487, §D5.4): writes to translation table base registers
  // require a full DSB ISH (not the store-only ISHST variant) so prior
  // table-page stores are observable to the table walker.
  __asm__ __volatile__("dsb ish");

  __asm__ __volatile__("msr ttbr0_el1, %0" ::"r"(l0_table_lo));
  __asm__ __volatile__("msr ttbr1_el1, %0" ::"r"(l0_table_hi));

  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  // invalidate TLBs
  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  // mmu enablement
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));

  sctlr |= (1 << 0);  // M = MMU enable
  sctlr |= (1 << 2);  // C = data cache
  sctlr |= (1 << 12); // I = instruction cache

  __asm__ __volatile__("msr sctlr_el1, %0" ::"r"(sctlr));
  __asm__ __volatile__("isb");

  uart_println("[MMU] Enabled");
  return l1_table;
}

// Forward decl: walk_levels is defined later but used by mmu_map_user_range.
static uint64_t *walk_levels(uint64_t *l0_table, uint64_t va, int target_level,
                             int alloc);


uint64_t *mmu_create_user_tables(void) {
  // Just an empty L0 (physical address) — walk_levels() will populate
  // L1/L2/L3 tables on demand when mmu_map_user_range() is called.
  return alloc_table();
}

void mmu_map_user_range(uint64_t *l0, uint64_t va, uint64_t pa,
                        uint64_t pages, uint64_t flags) {
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t *pte = walk_levels(l0, va + i * PAGE_SIZE, 3, 1);
    if (!pte) {
      uart_errorln("[MMU] mmu_map_user_range: walk failed");
      return;
    }
    // L3 page descriptors share their bit[1]=1 encoding with table
    // descriptors. Without PTE_TABLE the entry would be invalid.
    /* nG = 1: tag this entry with the current ASID so per-task contexts
     * are isolated by TTBR0_EL1[63:48] without needing a TLB flush on
     * context switch. Kernel mappings (TTBR1) deliberately leave nG=0 so
     * they remain global and visible to every ASID. */
    *pte = ((pa + i * PAGE_SIZE) & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE |
           PTE_AF | PTE_SH_INNER | PTE_NG | flags;
  }
}

void mmu_free_user_tables(uint64_t *l0_phys) {
  // Walk L0 → L1 → L2 → L3 and free every intermediate table page.
  // L3 page descriptors point to user data pages (text / stack), which are
  // freed separately by the scheduler.
  //
  // PTEs store physical addresses. Use PHYS_TO_VIRT to dereference them
  // since TTBR0 may not have an identity map when this runs.
  uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l0_phys);

  for (int i = 0; i < 512; i++) {
    if (!pte_valid(l0[i]))
      continue;

    uintptr_t l1_phys = (uintptr_t)pte_next_table(l0[i]);
    uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);

    for (int j = 0; j < 512; j++) {
      if (!pte_valid(l1[j]))
        continue;
      // bit[1] = 1 → table descriptor; bit[1] = 0 → 1 GB block (no L2 to free)
      if (!(l1[j] & PTE_TABLE))
        continue;

      uintptr_t l2_phys = (uintptr_t)pte_next_table(l1[j]);
      uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);

      for (int k = 0; k < 512; k++) {
        if (!pte_valid(l2[k]))
          continue;
        // bit[1] = 1 → L3 table; bit[1] = 0 → legacy 2 MB block
        if (!(l2[k] & PTE_TABLE))
          continue;

        uintptr_t l3_phys = (uintptr_t)pte_next_table(l2[k]);
        pmm_free_page(l3_phys);
      }

      pmm_free_page(l2_phys);
    }

    pmm_free_page(l1_phys);
  }

  pmm_free_page((uintptr_t)l0_phys);
}
// Walk the per-task L0 -> L1 -> L2 -> L3 tables, allocating intermediate
// table pages on demand when alloc=1. Returns a pointer (via TTBR1 high
// half) to the entry at `target_level` (2 = L2, 3 = L3).
//
// PTEs hold physical addresses, and table-pages themselves are referenced
// by physical address (alloc_table returns the pmm phys). After MMU enable,
// dereferencing a phys pointer through TTBR0 only works when TTBR0 happens
// to identity-map RAM. To be safe regardless of which task's TTBR0 is
// loaded, route every table read/write through the upper-half kernel
// mapping (TTBR1) via PHYS_TO_VIRT.
static uint64_t *walk_levels(uint64_t *l0_table, uint64_t va, int target_level,
                             int alloc) {
  uint64_t l0i = L0_INDEX(va);
  uint64_t l1i = L1_INDEX(va);
  uint64_t l2i = L2_INDEX(va);
  uint64_t l3i = L3_INDEX(va);

  uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l0_table);

  // L0 -> L1
  uint64_t *l1;
  if (!pte_valid(l0[l0i])) {
    if (!alloc)
      return 0;

    uint64_t *l1_phys = alloc_table();
    if (!l1_phys)
      return 0;

    l0[l0i] = (uint64_t)l1_phys | PTE_VALID | PTE_TABLE;
    l1 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l1_phys);
  } else {
    uintptr_t l1_phys = (uintptr_t)pte_next_table(l0[l0i]);
    l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);
  }

  // L1 -> L2
  uint64_t *l2;
  if (!pte_valid(l1[l1i])) {
    if (!alloc)
      return 0;

    uint64_t *l2_phys = alloc_table();
    if (!l2_phys)
      return 0;

    l1[l1i] = (uint64_t)l2_phys | PTE_VALID | PTE_TABLE;
    l2 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l2_phys);
  } else {
    uintptr_t l2_phys = (uintptr_t)pte_next_table(l1[l1i]);
    l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
  }

  if (target_level == 2) {
    return &l2[l2i];
  }

  // L2 -> L3  (target_level == 3)
  uint64_t *l3;
  if (!pte_valid(l2[l2i])) {
    if (!alloc)
      return 0;

    uint64_t *l3_phys = alloc_table();
    if (!l3_phys)
      return 0;

    l2[l2i] = (uint64_t)l3_phys | PTE_VALID | PTE_TABLE;
    l3 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l3_phys);
  } else {
    uintptr_t l3_phys = (uintptr_t)pte_next_table(l2[l2i]);
    l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
  }

  return &l3[l3i];
}

/* walk_page_table() removed: it was a thin wrapper around walk_levels()
 * with target_level=2, used only by a 2 MiB-block test that wrote into
 * a phys address treated as a VA and could clobber kernel code at PA
 * 0x40000000 if the PMM happened to hand back an early page. With user
 * mappings now living at L3 (4 KiB pages), there are no remaining
 * callers — walk_levels() is the canonical entry point. */

static void print_result(const char *name, int pass) {
  uart_printf("[MMU TEST] %s: %s\n", name, pass ? "PASS" : "FAIL");
}

static int test_mmu_enabled(void) {
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
  return (sctlr & 1);
}

static int test_identity_mapping(void) {
  uintptr_t page = pmm_allocate_page();
  if (!page) {
    uart_errorln(
        "[MMU TEST] Failed to allocate page for identity mapping test");
    return 0;
  }

  uint64_t *ptr = (uint64_t *)page;
  *ptr = 0xAABBCCDD;

  int pass = (*ptr == 0xAABBCCDD);

  pmm_free_page(page);
  return pass;
}

/* test_remap was removed: it was never invoked from mmu_run_tests and
 * mutated TTBR0 mappings in a way that's no longer safe with per-task L0s. */

/* L2 remap test: rewrite an L2 PTE in the kernel's TTBR0 identity-map
 * to point at a freshly-allocated 2 MiB region, write through the VA,
 * and verify the data lands at the new phys via the TTBR1 high-half.
 *
 * Prerequisite: TTBR0_EL1 must point at l0_table_lo (the boot-time
 * identity-mapping table) when this runs. mmu_run_tests() is invoked
 * from kernel_main right after mmu_init(), before any user task is
 * created, so this is naturally satisfied.
 *
 * pmm_allocate_pages returns 4 KiB-aligned RAM. To install a 2 MiB
 * block descriptor we need 2 MiB alignment, so we ask for 1024 pages
 * (4 MiB) and pick the 2 MiB-aligned half. The other half is freed. */
static int test_remap_l2(uint64_t *l1_table_phys) {
  uart_println("[MMU TEST] L2 remap test");

  /* l1_table is a *physical* pointer (returned by build_identity_tables
   * via alloc_table). Route every dereference through PHYS_TO_VIRT so the
   * test does not depend on TTBR0 happening to identity-map RAM. */
  uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l1_table_phys);

  uint64_t l1_idx = 1;  // safe RAM region (1 GiB — well past kernel)
  uint64_t l2_idx = 10; // arbitrary 2 MiB chunk

  /* The L1 entry is a TABLE descriptor; pte_next_table extracts the L2
   * physical pointer. Access via PHYS_TO_VIRT — same reason as above. */
  uintptr_t l2_phys = (uintptr_t)pte_next_table(l1[l1_idx]);
  uint64_t *l2      = (uint64_t *)PHYS_TO_VIRT(l2_phys);

  uint64_t old = l2[l2_idx];

  /* Get a 2 MiB-aligned chunk of RAM for the remap target. */
  uintptr_t alloc_phys = pmm_allocate_pages(1024);  // 4 MiB
  if (!alloc_phys) {
    uart_errorln("[MMU TEST] L2 remap: pmm_allocate_pages failed");
    return 0;
  }
  uintptr_t new_phys      = (alloc_phys + (_2MB - 1)) & ~(_2MB - 1);
  uint64_t  pre_pad_pages = (new_phys - alloc_phys) / PAGE_SIZE;
  uint64_t  post_pad_pages = 1024 - pre_pad_pages - 512;

  l2[l2_idx] = new_phys | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
               PTE_AP_RW | PTE_ATTRIDX(1);

  __asm__ __volatile__("tlbi vmalle1\n\tdsb ish\n\tisb");

  /* Intentional VA write: l1_idx*1 GiB + l2_idx*2 MiB lives in the TTBR0
   * lower half. With TTBR0=l0_table_lo (identity-mapped) at boot, the
   * write resolves through the freshly-installed PTE to new_phys. */
  uint64_t va = (l1_idx * _1GB) + (l2_idx * _2MB);
  *(volatile uint64_t *)va = 0xCAFEBABE;

  /* Verify via TTBR1 high-half mapping of new_phys — always reachable. */
  int pass = (*(volatile uint64_t *)PHYS_TO_VIRT(new_phys) == 0xCAFEBABE);

  /* Restore original mapping, flush, and return all pages. */
  l2[l2_idx] = old;
  __asm__ __volatile__("tlbi vmalle1\n\tdsb ish\n\tisb");

  if (pre_pad_pages)  pmm_free_pages(alloc_phys, pre_pad_pages);
  pmm_free_pages(new_phys, 512);
  if (post_pad_pages) pmm_free_pages(new_phys + (512 * PAGE_SIZE), post_pad_pages);

  return pass;
}

/* test_walk() removed alongside walk_page_table(): it install a 2 MiB
 * block at VA 0x50000000 pointing at a 2 MiB-aligned-DOWN phys range,
 * which could land at PA 0x40000000 (kernel .text) and silently clobber
 * code. Its essential coverage — that walk_levels() correctly populates
 * intermediate L1/L2/L3 tables — is now exercised on every boot by
 * sched_create_task() mapping per-task .text + user-stack via
 * mmu_map_user_range(). */

static int test_ttbr1_upper_half(void) {
  uart_println("[MMU TEST] TTBR1 upper half access test");

  uintptr_t pa = pmm_allocate_page();
  if (!pa) {
    uart_errorln("[MMU TEST] Failed to allocate page for TTBR1 test");
    return 0;
  }

  // Write value to lower half identity mapped address
  volatile uint64_t *lo_ptr = (volatile uint64_t *)pa;
  *lo_ptr = 0xABCDEFAD;
  __asm__ __volatile__("dsb ish");

  // Read back from the upper half address
  volatile uint64_t *hi_ptr = (volatile uint64_t *)PHYS_TO_VIRT(pa);

  uart_printf("[MMU TEST] lo_ptr=%x hi_ptr=%x\n", (uint64_t)lo_ptr,
              (uint64_t)hi_ptr);

  int pass = (*hi_ptr == 0xABCDEFAD);

  // also test writing to upper half and reading from lower half
  *hi_ptr = 0xABBCCCDD;
  __asm__ __volatile__("dsb ish");
  pass &= (*lo_ptr == 0xABBCCCDD);

  pmm_free_page(pa);
  return pass;
}

void mmu_run_tests(uint64_t *l1_table_phys) {
  print_result("MMU Enabled",      test_mmu_enabled());
  print_result("Identity Mapping", test_identity_mapping());
  print_result("L2 Remap",         test_remap_l2(l1_table_phys));
  print_result("TTBR1 Upper Half", test_ttbr1_upper_half());
}
