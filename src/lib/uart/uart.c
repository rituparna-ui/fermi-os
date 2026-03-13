#include "uart.h"
#include "./../mmio/mmio.h"

void uart_putc(const char c) { mmio_write8(UART_DR, c); }

void uart_puts(const char *str) {
  while (*str) {
    uart_putc(*str++);
  }
}
