#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "uart/uart.h"

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

  uint64_t *l0_table = (uint64_t *)pmm_allocate_page();
  if (!l0_table) {
    uart_errorln("[MMU] Failed to allocate L0 table");
    return 0;
  }

  uint64_t *l1_table = (uint64_t *)pmm_allocate_page();
  if (!l1_table) {
    uart_errorln("[MMU] Failed to allocate L1 table");
    return 0;
  }

  memset((void *)l0_table, 0, PAGE_SIZE);
  memset((void *)l1_table, 0, PAGE_SIZE);

  // VA range 0 → 512GB uses this L1 table
  l0_table[0] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;
  // table descriptor - bit[1:0]
  // 00/10 → invalid
  // 01 → block
  // 11 → table/page

  uint64_t mair = (0x00 << 0) | // AttrIdx 0 = Device memory
                  (0xff << 8);  // AttrIdx 1 = Normal memory

  __asm__ __volatile__("msr mair_el1, %0" ::"r"(mair));

  // Identity map L1 blocks - 8 * 1GB blocks
  for (uint64_t i = 0; i < 8; i++) {
    uint64_t phys = i * 0x40000000ULL;
    uint64_t attr;

    if (phys < 0x40000000ULL) {
      attr = 0; // device memory
    } else {
      attr = 1; // normal memory
    }

    l1_table[i] = phys | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
                  PTE_AP_RW | PTE_ATTRIDX(attr);
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
    uart_errorln("[MMU TEST] Failed to allocate page for identity mapping test");
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

void mmu_run_tests(uint64_t *l1_table) {
  print_result("MMU Enabled", test_mmu_enabled());
  print_result("Identity Mapping", test_identity_mapping());
  print_result("Remap Test", test_remap(l1_table));
}