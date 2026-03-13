#include "lib/uart/uart.h"

void kernel_main() {
  char *hello = "Hello !";

  uart_puts(hello);

  while (1) {
  }
}
