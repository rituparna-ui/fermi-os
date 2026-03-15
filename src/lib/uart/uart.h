#ifndef LIB_UART_H
#define LIB_UART_H

#include <stdint.h>
#define UART_BASE 0x9000000
#define UART_DR (UART_BASE + 0x00)
#define UART_FR (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR (UART_BASE + 0x30)
#define UART_ICR (UART_BASE + 0x44)

void uart_init(void);

void uart_putc(const char c);
uint8_t uart_getc(void);
void uart_puts(const char *str);
void uart_println(const char *str);

#endif // !LIB_UART_H
