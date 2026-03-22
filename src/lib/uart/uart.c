#include "uart.h"
#include "mmio/mmio.h"

// PL011
void uart_init() {
  // disable uart
  mmio_write32(UART_CR, 0x00000000);

  // clear pending interrupts
  mmio_write32(UART_ICR, 0x7FF);

  // setup baudrate
  // divisor = clk/(16 * baud)
  // clk = 24000000/(16 * 115200) = 13.02083333
  // integer = 13
  // fraction = 0.02083333
  // fraction register = round(0.02083333 * 2^6) = 2
  // FBRD is a 6 bit number
  mmio_write32(UART_IBRD, 13);
  mmio_write32(UART_FBRD, 2);

  // enable FIFO, 8bit data transmission - 1 stop bit, no parity
  mmio_write32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

  // enable UART, RX, TX
  mmio_write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));

  uart_println("UART Initialized !");
}

void uart_putc(const char c) {
  // check if transmit fifo is full - TXFF
  while (mmio_read32(UART_FR) & (1 << 5)) {
  }

  mmio_write32(UART_DR, c);
  return;
}

uint8_t uart_getc() {
  // check if receive fifo is empty - RXFE
  while (mmio_read32(UART_FR) & (1 << 4)) {
  }

  uint8_t value = (uint8_t)mmio_read32(UART_DR);
  return value;
}

void uart_puts(const char *str) {
  while (*str) {
    uart_putc(*str++);
  }
}

void uart_println(const char *str) {
  uart_puts(str);
  uart_putc('\n');
}

void uart_errorln(const char *err) {
  uart_puts("[ERROR!]: ");
  uart_puts(err);
  uart_putc('\n');
}

void uart_puthex(uint64_t value) {
  uart_puts("0x");
  int started = 0;
  for (int i = 60; i >= 0; i -= 4) {
    uint8_t nibble = (value >> i) & 0xF;
    if (nibble || started || i == 0) {
      uart_putc(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
      started = 1;
    }
  }
}

void uart_putdec(uint64_t value) {
  if (value == 0) {
    uart_putc('0');
    return;
  }
  char buf[20];
  int i = 0;
  while (value) {
    buf[i++] = '0' + (value % 10);
    value /= 10;
  }
  while (i--) {
    uart_putc(buf[i]);
  }
}

void uart_putbin(uint64_t value) {
  uart_puts("0b");
  int started = 0;
  for (int i = 63; i >= 0; i--) {
    uint8_t bit = (value >> i) & 1;
    if (bit || started || i == 0) {
      uart_putc('0' + bit);
      started = 1;
    }
  }
}
