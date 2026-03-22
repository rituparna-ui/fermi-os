#include "uart/uart.h"

void kernel_main() {
  char *str = "FermiOS - Booting Up...";

  uart_puts(str);

  while (1) {
  }
}
