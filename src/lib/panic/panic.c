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
    uart_printf("  Reason: %s\n", msg);
  }

  // Dump system registers
  uint64_t elr, esr, far_reg, sp;

  __asm__ __volatile__("mrs %0, elr_el1" : "=r"(elr));
  __asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
  __asm__ __volatile__("mrs %0, far_el1" : "=r"(far_reg));
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));

  uart_println("");

  uart_printf("  ELR_EL1 (return addr) : %x\n", elr);
  uart_printf("  ESR_EL1 (syndrome)    : %x\n", esr);
  uart_printf("  FAR_EL1 (fault addr)  : %x\n", far_reg);
  uart_printf("  SP      (stack ptr)   : %x\n", sp);
  uart_printf("\n  System halted. Reset to continue.\n");

  // Halt CPU
  while (1) {
    __asm__ __volatile__("wfe");
  }
}
