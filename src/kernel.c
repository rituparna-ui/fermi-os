#include "exception.h"
#include "gic/gic.h"
#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "mmio/mmio.h"
#include "panic/panic.h"
#include "sched/sched.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "timer/timer.h"
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

static void task_a(void) {
  for (int i = 0; i < 5; i++) {
    uart_printf("[Task A] iteration %d\n", (uint64_t)i);
    sleep_ms(200);
  }
  uart_println("[Task A] done! exiting");
}

static void task_b(void) {
  while (1) {
    uart_println("[Task B] running");
    sleep_ms(500);
  }
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

  sched_init();
  sched_create_task("task_a", task_a);
  sched_create_task("task_b", task_b);

  timer_init();
  timer_start(TIMER_INTERVAL_MS);

  uart_println("[KERNEL] Ready! running idle task...");

  while (1) {
    __asm__ __volatile__("wfi");
  }
}

void kernel_panic_return(void) {
  kernel_panic("kernel_main returned unexpectedly");
}
