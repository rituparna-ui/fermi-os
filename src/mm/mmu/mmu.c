#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

static uint64_t *l0_table;

static uint64_t *alloc_table() {
  uint64_t *table = (uint64_t *)pmm_allocate_page();
  if (!table) {
    uart_errorln("[MMU] Failed to allocate table");
    return 0;
  }
  memset(table, 0, PAGE_SIZE);
  return table;
}

uint64_t *mmu_init() {
  // reference manual - section D8.3
  uart_println("[MMU] Initializing MMU (48 bit VAS, 4kb granule)");
  // VA layout:
  // [47:39] → L0 (9 bits)
  // [38:30] → L1 (9 bits)
  // [29:21] → L2 (9 bits)
  // [20:12] → L3 (9 bits)
  // [11:0]  → offset
  // Each level has 512 entries (2^9)

  l0_table = alloc_table();
  if (!l0_table) {
    return 0;
  }

  uint64_t *l1_table = alloc_table();
  if (!l1_table) {
    return 0;
  }

  // VA range 0 → 512GB uses this L1 table
  l0_table[0] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;
  // table descriptor - bit[1:0]
  // 00/10 → invalid
  // 01 → block
  // 11 → table/page

  uint64_t mair = (0x00 << 0) | // AttrIdx 0 = Device memory
                  (0xff << 8);  // AttrIdx 1 = Normal memory

  __asm__ __volatile__("msr mair_el1, %0" ::"r"(mair));

  // Identity map using L2 tables with 2MB blocks
  for (uint64_t l1i = 0; l1i < 8; l1i++) {
    uint64_t *l2_table = alloc_table();
    if (!l2_table) {
      return 0;
    }

    l1_table[l1i] = (uint64_t)l2_table | PTE_VALID | PTE_TABLE;

    for (uint64_t l2i = 0; l2i < 512; l2i++) {
      uint64_t phys_addr = l1i * _1GB + l2i * _2MB;
      uint64_t attr = (phys_addr < MEM_START) ? 0 : 1; // 0=device, 1=normal

      l2_table[l2i] = phys_addr | PTE_VALID | PTE_BLOCK | PTE_AF |
                      PTE_SH_INNER | PTE_AP_RW | PTE_ATTRIDX(attr);
    }
  }
  // https://developer.arm.com/documentation/100095/0002/system-control/aarch64-register-descriptions/translation-control-register--el1
  uint64_t tcr =
      (16ULL << 0) |    // T0SZ = 16 → 48-bit VA
      (0b01ULL << 8) |  // IRGN0 = Write-Back, Write-Allocate
      (0b01ULL << 10) | // ORGN0 = Write-Back, Write-Allocate
      (0b11ULL << 12) | // SH0 = inner shareable
      (0b00ULL << 14) | // TG0 = 4KB granule
      (0b010ULL << 32); // IPS = 40-bit PA (1TB) (needed for >4GB RAM)

  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(tcr));

  __asm__ __volatile__("msr ttbr0_el1, %0" ::"r"(l0_table));
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
  uart_puts("[MMU TEST] ");
  uart_puts(name);
  uart_puts(": ");
  uart_println(pass ? "PASS" : "FAIL");
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
  uart_puts("[MMU TEST] va1=");
  uart_puthex((uint64_t)va1);
  uart_puts(" va2=");
  uart_puthex((uint64_t)va2);
  uart_println("");

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

void mmu_run_tests(uint64_t *l1_table) {
  print_result("MMU Enabled", test_mmu_enabled());
  print_result("Identity Mapping", test_identity_mapping());
  print_result("Remap Test L2", test_remap_l2(l1_table));
  print_result("MMU Table Walk", test_walk(l0_table));
}
