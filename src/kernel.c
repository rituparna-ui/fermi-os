#include "lib/mmio/mmio.h"
#include <stdint.h>

#define UART_BASE 0x9000000
#define UART_DR (UART_BASE + 0x00)

void kernel_main() {
  char *hello = "Hello !";

  while (*hello) {
    mmio_write8(UART_DR, *hello++);
  }

  while (1) {
  }
}
