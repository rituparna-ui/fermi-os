#include "pmm/pmm.h"
#include "uart/uart.h"
#include "utils/utils.h"

void kernel_main() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  pmm_init(MEM_START, MEM_SIZE);

  while (1) {
    uart_putc(uart_getc());
  }
}
