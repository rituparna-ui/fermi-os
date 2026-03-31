#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include <stdint.h>

// running in PAS
void early_init() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  pmm_init(MEM_START, MEM_SIZE);
  pmm_print_info();

  mmu_init();
  // mmu_run_tests(l1);

  uart_println("[BOOT] MMU Enabled. Jumping to Upper Half");
}

// runs in VAS Upper Half after boot.S relocates program counter and stack
// pointer
void kernel_main() {
  // Verify if the kernel is running in upper half
  uart_puts("[KERNEL] kernel_main address: ");
  uart_puthex((uint64_t)(uintptr_t)kernel_main);
  uart_println("");

  // verify stack pointer in upper half
  uint64_t sp;
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));
  uart_puts("[KERNEL] Stack Pointer: ");
  uart_puthex(sp);
  uart_println("");

  uart_println("[KERNEL] Ready ! Entering echo loop");

  while (1) {
    uart_putc(uart_getc());
  }
}
