#include "lib/uart/uart.h"

void kernel_main() {
  uart_init();
  uart_println("Fermi OS - Booting up !");

  while (1) {
    uart_putc(uart_getc());
  }
}
