#include "uart.h"
#include "mmio/mmio.h"

void uart_init(void) {
  // disable uart
  mmio_write16(UART_CR, 0x00);

  // clear pending interrupts
  mmio_write16(UART_ICR, 0x7FF);

  // setup baudrate
  // baudrate divisor = clk/(16 * baud)
  // clk = 24mhz; baud = 115200
  // divisor = 24000000/(16×115200) = 13.020833
  // integer = 13
  // fractional part = 0.020833
  // fraction register = round(0.020833 * 64) = 1
  mmio_write16(UART_IBRD, 13);
  mmio_write8(UART_FBRD, 1);

  // enable fifo, 8 bit data, 1 stop bit, no parity
  mmio_write16(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

  // Enable UART, RX, TX
  mmio_write16(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
  uart_puts("UART Initialized !\n");
}

void uart_putc(const char c) {
  // check if transmit fifo is full - TXFF
  while (mmio_read16(UART_FR) & (1 << 5)) {
  }

  mmio_write8(UART_DR, c);
}

uint8_t uart_getc(void) {
  // check if receive fifo is empty - RXFE
  while (mmio_read16(UART_FR) & (1 << 4)) {
  }

  return mmio_read8(UART_DR);
}

void uart_puts(const char *str) {
  while (*str) {
    uart_putc(*str++);
  }
}

void uart_println(const char *str) {
  uart_puts(str);
  uart_puts("\n");
}

void uart_puthex(uint64_t value) {
  const char hex[] = "0123456789ABCDEF";

  uart_puts("0x");

  for (int i = 60; i >= 0; i -= 4) {
    uint8_t digit = (value >> i) & 0xF;
    uart_putc(hex[digit]);
  }
}

void uart_putdec(uint64_t value) {
  char buf[21]; // enough for uint64
  int i = 0;

  if (value == 0) {
    uart_putc('0');
    return;
  }

  while (value > 0) {
    buf[i++] = '0' + (value % 10);
    value /= 10;
  }

  while (i--) {
    uart_putc(buf[i]);
  }
}

void uart_putbin(uint64_t value) {
  uart_puts("0b");

  for (int i = 63; i >= 0; i--) {
    uart_putc((value & (1ULL << i)) ? '1' : '0');
  }
}
