#include "panic.h"
#include "uart/uart.h"
#include <stdint.h>

__attribute__((noreturn)) void kernel_panic(const char *msg) {
  // Mask all interrupts (D, A, I, F) immediately so a pending IRQ cannot
  // re-enter the exception path during the diagnostic dump and recurse.
  __asm__ __volatile__("msr daifset, #0xf" ::: "memory");

  // Capture the caller's return address before any C function call below
  // can clobber x30. __builtin_return_address(0) is the AAPCS64-correct
  // portable way to read it.
  uint64_t caller_lr = (uint64_t)__builtin_return_address(0);

  uart_println("");
  uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  uart_println("!!!         KERNEL PANIC            !!!");
  uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  uart_println("");

  if (msg) {
    uart_printf("  Reason: %s\n", msg);
  }

  // Dump system registers
  uint64_t elr, esr, far_reg, sp, spsr;

  __asm__ __volatile__("mrs %0, elr_el1" : "=r"(elr));
  __asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
  __asm__ __volatile__("mrs %0, far_el1" : "=r"(far_reg));
  __asm__ __volatile__("mrs %0, spsr_el1" : "=r"(spsr));
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));

  uart_println("");

  uart_printf("  ELR_EL1  (return addr) : %x\n", elr);
  uart_printf("  ESR_EL1  (syndrome)    : %x\n", esr);
  uart_printf("  FAR_EL1  (fault addr)  : %x\n", far_reg);
  uart_printf("  SPSR_EL1 (saved state) : %x\n", spsr);
  uart_printf("  SP       (stack ptr)   : %x\n", sp);
  uart_printf("  LR       (caller pc)   : %x\n", caller_lr);
  uart_printf("\n  System halted. Reset to continue.\n");

  // Halt CPU. With DAIF masked above, wfi cannot be woken by IRQ/FIQ/SError
  // and the CPU stays parked.
  while (1) {
    __asm__ __volatile__("wfi");
  }
}
