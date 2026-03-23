#ifndef LIB_UART_H
#define LIB_UART_H

#include <stdint.h>

#define UART_BASE 0x09000000UL
#define UART_DR (UART_BASE + 0x00)
#define UART_FR (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR (UART_BASE + 0x30)
#define UART_ICR (UART_BASE + 0x44)

#define UART_DISABLE 0x00000000UL
#define UART_CLR_PENDING_INT 0x7FF
// divisor = clk/(16 * baud)
// clk = 24000000/(16 * 115200) = 13.02083333
// integer = 13
// fraction = 0.02083333
// fraction register = round(0.02083333 * 2^6) = 2
// FBRD is a 6 bit number
#define UART_BAUD_INT 13
#define UART_BAUD_FRAC 2
#define UART_ENABLE_FIFO (1 << 4)
#define UART_8_BIT_DATA (1 << 5) | (1 << 6)
#define UART_ENABLE (1 << 0)
#define UART_TX_ENABLE (1 << 8)
#define UART_RX_ENABLE (1 << 9)
#define UART_TXFF (1 << 5)
#define UART_RXFE (1 << 4)

void uart_init(void);
void uart_putc(const char c);
uint8_t uart_getc(void);
void uart_puts(const char *str);
void uart_println(const char *str);
void uart_errorln(const char *err);

void uart_puthex(uint64_t value);
void uart_putdec(uint64_t value);
void uart_putbin(uint64_t value);

#endif