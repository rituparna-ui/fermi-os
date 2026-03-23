#include "uart.h"
#include "mmio/mmio.h"

// PL011
void uart_init() {
  // disable uart
  mmio_write32(UART_CR, UART_DISABLE);

  // clear pending interrupts
  mmio_write32(UART_ICR, UART_CLR_PENDING_INT);

  // setup baudrate
  // uart.h for calculation
  mmio_write32(UART_IBRD, UART_BAUD_INT);
  mmio_write32(UART_FBRD, UART_BAUD_FRAC);

  // enable FIFO, 8bit data transmission - 1 stop bit, no parity
  mmio_write32(UART_LCRH, UART_ENABLE_FIFO | UART_8_BIT_DATA);

  // enable UART, RX, TX
  mmio_write32(UART_CR, UART_ENABLE | UART_TX_ENABLE | UART_RX_ENABLE);

  uart_println("UART Initialized !");
}

void uart_putc(const char c) {
  // check if transmit fifo is full - TXFF
  while (mmio_read32(UART_FR) & UART_TXFF) {
  }

  mmio_write32(UART_DR, c);
  return;
}

uint8_t uart_getc() {
  // check if receive fifo is empty - RXFE
  while (mmio_read32(UART_FR) & UART_RXFE) {
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
