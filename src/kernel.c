#include <stdint.h>

#define UART_BASE 0x09000000UL

void kernel_main() {
  char *str = "Hello !";

  while (*str) {
    *(volatile uint32_t *)UART_BASE = *str++;
  }

  while (1) {
  }
}
