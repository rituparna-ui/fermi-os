#include "uart/uart.h"
#include "utils/utils.h"

void kernel_main() {
  uart_init();
  uart_println("Fermi OS - Booting up !");

  print_current_el();

  while (1) {
    uart_putc(uart_getc());
  }
}
