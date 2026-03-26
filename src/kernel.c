#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include <stdint.h>

/*
 * kernel_main — entry point after boot.S has:
 *   1. Built identity-mapped page tables (TTBR0)
 *   2. Built higher-half page tables     (TTBR1)
 *   3. Enabled the MMU
 *   4. Jumped here at its virtual (0xFFFF...) address
 *
 * From this point on we run entirely in the upper half.
 * TTBR0 identity map is still present (useful for early debug;
 * can be torn down later when user-space is added).
 */
void kernel_main(void) {
  /* UART is now at its higher-half virtual address */
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  uart_println("[BOOT] Running in higher-half kernel (0xFFFF...)");
  print_current_el();

  /* PMM works with physical addresses internally;
   * bitmap is accessed through its TTBR1 virtual address. */
  pmm_init(MEM_START, MEM_SIZE);
  pmm_print_info();

  /* Print MMU / TTBR status */
  mmu_init_kernel_tables();

  /* Run basic sanity tests */
  mmu_run_tests();

  uart_println("[BOOT] Kernel initialisation complete.");

  while (1) {
    uart_putc(uart_getc());
  }
}
