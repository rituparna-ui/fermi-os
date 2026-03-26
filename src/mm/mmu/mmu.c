#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "mm/vm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

/*
 * After boot.S has enabled the MMU with coarse 1 GB block mappings
 * in both TTBR0 and TTBR1, the kernel can optionally build finer-
 * grained page tables here (e.g. 2 MB or 4 KB pages for TTBR1).
 *
 * For now we keep the boot-time 1 GB block mappings and provide
 * helpers for future use.
 */

/*
 * alloc_table — allocate a zeroed page-table page.
 * PMM returns a physical address; we need a virtual pointer.
 */
static uint64_t *alloc_table(void) {
  uint64_t pa = pmm_allocate_page();
  if (!pa) {
    uart_errorln("[MMU] Failed to allocate table");
    return 0;
  }
  uint64_t *va = (uint64_t *)PHYS_TO_VIRT(pa);
  memset(va, 0, PAGE_SIZE);
  return va;
}

/*
 * mmu_init_kernel_tables — placeholder for when you want to replace
 * the coarse 1 GB boot-time mappings with 2 MB or 4 KB pages under
 * TTBR1.  For now the boot mappings are sufficient.
 */
void mmu_init_kernel_tables(void) {
  uart_println("[MMU] Kernel running in higher-half (boot tables active)");

  /* Read back TTBR values to confirm */
  uint64_t ttbr0, ttbr1;
  __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
  __asm__ __volatile__("mrs %0, ttbr1_el1" : "=r"(ttbr1));

  uart_puts("[MMU] TTBR0_EL1 = ");
  uart_puthex(ttbr0);
  uart_println("");
  uart_puts("[MMU] TTBR1_EL1 = ");
  uart_puthex(ttbr1);
  uart_println("");

  uint64_t tcr;
  __asm__ __volatile__("mrs %0, tcr_el1" : "=r"(tcr));
  uart_puts("[MMU] TCR_EL1   = ");
  uart_puthex(tcr);
  uart_println("");
}

/*
 * Walk L0 → L1 → L2 for a given VA.
 * Returns a pointer (virtual) to the L2 entry, optionally allocating
 * intermediate tables.  Table entries store *physical* addresses;
 * we convert to virtual when dereferencing.
 */
uint64_t *walk_page_table(uint64_t *l0_table, uint64_t va, int alloc) {
  uint64_t l0i = L0_INDEX(va);
  uint64_t l1i = L1_INDEX(va);
  uint64_t l2i = L2_INDEX(va);

  /* L0 → L1 */
  uint64_t *l1_table;

  if (!pte_valid(l0_table[l0i])) {
    if (!alloc)
      return 0;

    l1_table = alloc_table();
    if (!l1_table)
      return 0;

    /* Store physical address in the PTE */
    l0_table[l0i] = KVA_TO_PA(l1_table) | PTE_VALID | PTE_TABLE;
  } else {
    /* PTE contains PA — convert to VA for access */
    l1_table = (uint64_t *)PHYS_TO_VIRT((uint64_t)pte_next_table(l0_table[l0i]));
  }

  /* L1 → L2 */
  uint64_t *l2_table;

  if (!pte_valid(l1_table[l1i])) {
    if (!alloc)
      return 0;

    l2_table = alloc_table();
    if (!l2_table)
      return 0;

    l1_table[l1i] = KVA_TO_PA(l2_table) | PTE_VALID | PTE_TABLE;
  } else {
    l2_table = (uint64_t *)PHYS_TO_VIRT((uint64_t)pte_next_table(l1_table[l1i]));
  }

  return &l2_table[l2i];
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void print_result(const char *name, int pass) {
  uart_puts("[MMU TEST] ");
  uart_puts(name);
  uart_puts(": ");
  uart_println(pass ? "PASS" : "FAIL");
}

static int test_mmu_enabled(void) {
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
  return (sctlr & 1);
}

static int test_higher_half(void) {
  /*
   * If we're executing this function, our PC should be in the
   * upper-half range (0xFFFF...).
   */
  uint64_t pc;
  __asm__ __volatile__("adr %0, ." : "=r"(pc));
  return (pc >= 0xFFFF000000000000ULL);
}

static int test_identity_mapping(void) {
  /*
   * Allocate a physical page, access it through its
   * higher-half virtual address.
   */
  uintptr_t pa = pmm_allocate_page();
  if (!pa) {
    uart_errorln("[MMU TEST] Failed to allocate page");
    return 0;
  }

  uint64_t *va = (uint64_t *)PHYS_TO_VIRT(pa);
  *va = 0xAABBCCDD;

  int pass = (*va == 0xAABBCCDD);

  pmm_free_page(pa);
  return pass;
}

void mmu_run_tests(void) {
  print_result("MMU Enabled", test_mmu_enabled());
  print_result("Higher-Half PC", test_higher_half());
  print_result("KVA Read/Write", test_identity_mapping());
}
