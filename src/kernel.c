#include <stdint.h>

#define UART_BASE 0x9000000
#define UART_DR ((volatile uint32_t *)(UART_BASE + 0x00))

void kernel_main() {
  *UART_DR = 'H';
  *UART_DR = 'e';
  *UART_DR = 'l';
  *UART_DR = 'l';
  *UART_DR = 'o';
  *UART_DR = '!';

  while (1) {
  }
}
