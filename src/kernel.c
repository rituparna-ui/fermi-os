#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "mmio/mmio.h"
#include "panic/panic.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"
#include "utils/utils.h"
#include <stdint.h>

extern uint8_t __bss_start;
extern uint8_t __bss_end;

static void zero_bss(void) {
  memset(&__bss_start, 0, (size_t)(&__bss_end - &__bss_start));
}

// running in PAS
void early_init() {
  zero_bss();

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
  // all device access through TTBR1
  mmio_switch_to_upper();

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

  heap_init();

  uart_println("[KERNEL] Ready ! Entering echo loop");

  while (1) {
    uart_putc(uart_getc());
  }
}

void kernel_panic_return(void) {
  kernel_panic("kernel_main returned unexpectedly");
}
