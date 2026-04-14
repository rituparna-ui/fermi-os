#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

static uint64_t *l0_table_lo;
static uint64_t *l0_table_hi;

static uint64_t *alloc_table() {
  uint64_t *table = (uint64_t *)pmm_allocate_page();
  if (!table) {
    uart_errorln("[MMU] Failed to allocate table");
    return 0;
  }
  memset(table, 0, PAGE_SIZE);
  return table;
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
      (0b010ULL << 32); // IPS = 40-bit PA (1TB) (needed for >4GB RAM)

  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(tcr));
  __asm__ __volatile__("dsb ishst");

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

uint64_t *mmu_create_user_tables(void) {
  return alloc_table(); // just an empty L0 — walk_page_table will fill on
                        // demand
}

void mmu_map_user_page(uint64_t *l0, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pte = walk_page_table(l0, va, 1);
  if (!pte) {
    uart_errorln("[MMU] Failed to map user page");
    return;
  }
  *pte = (pa & ~(_2MB - 1)) | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
         flags;
}

void mmu_free_user_tables(uint64_t *l0_phys) {
  // Walk L0 → L1 → L2 and free all intermediate table pages.
  // L2 entries are 2MB block descriptors — the physical blocks they point to
  // (user text, user stack) are freed separately by the scheduler.
  //
  // PTEs store physical addresses. use PHYS_TO_VIRT to dereference
  // them since TTBR0 may not have an identity map when this runs.
  uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT((uintptr_t)l0_phys);

  for (int i = 0; i < 512; i++) {
    if (!pte_valid(l0[i]))
      continue;

    uintptr_t l1_phys = (uintptr_t)pte_next_table(l0[i]);
    uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);

    for (int j = 0; j < 512; j++) {
      if (!pte_valid(l1[j]))
        continue;

      // Only free if this is a table entry pointing to an L2 page
      // (bit[1] = 1 == table; bit[1] = 0 == 1GB block)
      if (l1[j] & PTE_TABLE) {
        uintptr_t l2_phys = (uintptr_t)pte_next_table(l1[j]);
        pmm_free_page(l2_phys);
      }
    }

    pmm_free_page(l1_phys);
  }

  pmm_free_page((uintptr_t)l0_phys);
}

// Walk L0 → L1 → L2
// Return a pointer to L2 entry
uint64_t *walk_page_table(uint64_t *l0_table, uint64_t va, int alloc) {
  uint64_t l0i = L0_INDEX(va);
  uint64_t l1i = L1_INDEX(va);
  uint64_t l2i = L2_INDEX(va);

  // L0 Table
  uint64_t *l1_table;

  if (!pte_valid(l0_table[l0i])) {
    if (!alloc)
      return 0;

    l1_table = alloc_table();
    if (!l1_table)
      return 0;

    l0_table[l0i] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;
  } else {
    l1_table = pte_next_table(l0_table[l0i]);
  }

  // L1 Table
  uint64_t *l2_table;

  if (!pte_valid(l1_table[l1i])) {
    if (!alloc)
      return 0;

    l2_table = alloc_table();
    if (!l2_table)
      return 0;

    l1_table[l1i] = (uint64_t)l2_table | PTE_VALID | PTE_TABLE;
  } else {
    l2_table = pte_next_table(l1_table[l1i]);
  }

  // L2 Table
  return &l2_table[l2i];
}

static void print_result(const char *name, int pass) {
  uart_printf("[MMU TEST] %s: %s\n", name, pass ? "PASS" : "FAIL");
}

int test_mmu_enabled() {
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
  return (sctlr & 1);
}

int test_identity_mapping() {
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

int test_remap(uint64_t *l1_table) {
  uart_println("[MMU TEST] Running SAFE remap test");

  uint64_t idx1 = 6;
  uint64_t idx2 = 7;

  uint64_t *va1 = (uint64_t *)(idx1 * 0x40000000ULL);
  uint64_t *va2 = (uint64_t *)(idx2 * 0x40000000ULL);

  // Before swap: va2 → PA 0x1C0000000 (identity mapped)
  // Write a known value to PA 0x1C0000000 via va2
  *va2 = 0xABABAABB;
  __asm__ __volatile__("dsb ish");

  uint64_t old1 = l1_table[idx1];
  uint64_t old2 = l1_table[idx2];

  // Swap mappings: va1 → PA 0x1C0000000, va2 → PA 0x180000000
  l1_table[idx1] = old2;
  l1_table[idx2] = old1;

  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  // After swap, va1 should now point to PA 0x1C0000000
  // which contains 0xABABAABB
  uart_printf("[MMU TEST] va1=%x va2=%x\n", (uint64_t)va1, (uint64_t)va2);

  int pass = (*va1 == 0xABABAABB);

  // Restore original mappings
  l1_table[idx1] = old1;
  l1_table[idx2] = old2;

  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  return pass;
}

int test_remap_l2(uint64_t *l1_table) {
  uart_println("[MMU TEST] L2 remap test");

  uint64_t l1_idx = 1;  // safe RAM region
  uint64_t l2_idx = 10; // arbitrary 2MB chunk

  uint64_t *l2_table = (uint64_t *)(l1_table[l1_idx] & ~0xFFFULL);

  uint64_t old = l2_table[l2_idx];

  uint64_t new_phys = ((l1_idx * 0x40000000ULL) + ((l2_idx + 1) * 0x200000ULL));

  l2_table[l2_idx] = new_phys | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
                     PTE_AP_RW | PTE_ATTRIDX(1);

  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  uint64_t va = (l1_idx * 0x40000000ULL) + (l2_idx * 0x200000ULL);

  uint64_t *ptr = (uint64_t *)va;
  uint64_t *phys_ptr = (uint64_t *)new_phys;

  *ptr = 0xCAFEBABE;

  int pass = (*phys_ptr == 0xCAFEBABE);

  // restore
  l2_table[l2_idx] = old;

  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  return pass;
}

int test_walk(uint64_t *l0) {
  uart_println("[MMU WALK TEST]");

  uint64_t va = 0x50000000;

  uint64_t pa = pmm_allocate_page();
  uint64_t aligned_pa = pa & 0xFFFFFFE00000ULL;

  uint64_t *pte = walk_page_table(l0, va, 1);

  *pte = aligned_pa | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
         PTE_AP_RW | PTE_ATTRIDX(1);

  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  uint64_t *ptr = (uint64_t *)va;

  *ptr = 0x12345678;

  uint64_t *check = (uint64_t *)(aligned_pa + (va & 0x1FFFFF));

  return *check == 0x12345678;
}

int test_ttbr1_upper_half() {
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

void mmu_run_tests(uint64_t *l1_table) {
  print_result("MMU Enabled", test_mmu_enabled());
  print_result("Identity Mapping", test_identity_mapping());
  print_result("Remap Test L2", test_remap_l2(l1_table));
  print_result("MMU Table Walk", test_walk(l0_table_lo));
  print_result("TTBR1 Upper Half", test_ttbr1_upper_half());
}
