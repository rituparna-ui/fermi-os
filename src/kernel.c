#include "pmm/pmm.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include <stdint.h>

void kernel_main() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  pmm_init(MEM_START, MEM_SIZE);
  pmm_print_info();

  while (1) {
    uart_putc(uart_getc());
  }
}
