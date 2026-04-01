#include "panic.h"
#include "uart/uart.h"
#include <stdint.h>

__attribute__((noreturn)) void kernel_panic(const char *msg) {
  uart_println("");
  uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  uart_println("!!!         KERNEL PANIC            !!!");
  uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  uart_println("");

  if (msg) {
    uart_puts("  Reason: ");
    uart_println(msg);
  }

  // Dump system registers
  uint64_t elr, esr, far_reg, sp;

  __asm__ __volatile__("mrs %0, elr_el1" : "=r"(elr));
  __asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
  __asm__ __volatile__("mrs %0, far_el1" : "=r"(far_reg));
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));

  uart_println("");
  uart_puts("  ELR_EL1 (return addr) : ");
  uart_puthex(elr);
  uart_println("");

  uart_puts("  ESR_EL1 (syndrome)    : ");
  uart_puthex(esr);
  uart_println("");

  uart_puts("  FAR_EL1 (fault addr)  : ");
  uart_puthex(far_reg);
  uart_println("");

  uart_puts("  SP      (stack ptr)   : ");
  uart_puthex(sp);
  uart_println("");

  uart_println("");
  uart_println("  System halted. Reset to continue.");
  uart_println("");

  // Halt CPU
  while (1) {
    __asm__ __volatile__("wfe");
  }
}
