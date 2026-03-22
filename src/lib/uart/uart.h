#ifndef LIB_UART_H
#define LIB_UART_H

#define UART_BASE 0x09000000UL
#define UART_DR (UART_BASE + 0x00)

void uart_putc(const char c);
void uart_puts(const char *str);

#endif