#include "uart/uart.h"

void kernel_main() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");

  while (1) {
    uart_putc(uart_getc());
  }
}
