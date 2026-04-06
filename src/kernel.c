#include "exception.h"
#include "gic/gic.h"
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

void enable_fp_simd() {
  // CPACR_EL1.FPEN = 0b11
  // GCC uses SIMD registers for varargs
  // got ESR_EL1 : 0x1FE00000 while building uart_printf
  uint64_t cpacr;
  __asm__ __volatile__("mrs %0, cpacr_el1" : "=r"(cpacr));
  cpacr |= (3ULL << 20);
  __asm__ __volatile__("msr cpacr_el1, %0" ::"r"(cpacr));
  __asm__ __volatile__("isb");
}

// running in PAS
void early_init() {
  zero_bss();
  enable_fp_simd();

  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  exceptions_init();

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

  // relocate VBAR_EL1 to upper half
  exceptions_init_upper();

  // Verify if the kernel is running in upper half
  uart_printf("[KERNEL] kernel_main address: %x\n",
              (uint64_t)(uintptr_t)kernel_main);

  // verify stack pointer in upper half
  uint64_t sp;
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));
  uart_printf("[KERNEL] Stack Pointer: %x\n", sp);

  heap_init();

  gic_init();

  {
    // Enable timer, fire every ~1 second
    uint64_t freq;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ __volatile__("msr cntp_tval_el0, %0" ::"r"(freq));
    __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(1ULL)); // enable

    // PPI 30 = physical timer
    gic_enable_irq(30);
  }

  uart_println("[KERNEL] Ready ! Entering echo loop");

  while (1) {
    uart_putc(uart_getc());
  }
}

void kernel_panic_return(void) {
  kernel_panic("kernel_main returned unexpectedly");
}
