#include "vuart.h"
#include "uart/uart.h"

/* PL011 register offsets (from the guest's UART window base). */
#define VUART_BASE 0x09000000ULL
#define VUART_SIZE 0x1000ULL
#define R_DR   0x00 /* data register */
#define R_FR   0x18 /* flag register */
#define R_IBRD 0x24
#define R_FBRD 0x28
#define R_LCRH 0x2C
#define R_CR   0x30
#define R_ICR  0x44

/* Flag register bits the guest polls. */
#define FR_RXFE (1U << 4) /* receive FIFO empty  */
#define FR_TXFF (1U << 5) /* transmit FIFO full  */

int vuart_is_target(uint64_t ipa) {
  return ipa >= VUART_BASE && ipa < (VUART_BASE + VUART_SIZE);
}

void vuart_init(vuart_t *u, const char *name) {
  u->name = name;
  u->len = 0;
  u->at_line_start = 1;
}

/* Emit a "[name] " prefix to the host UART once per output line. */
static void vuart_prefix(vuart_t *u) {
  if (u->at_line_start) {
    uart_putc('[');
    uart_puts(u->name);
    uart_puts("] ");
    u->at_line_start = 0;
  }
}

/* Push one guest-emitted char into the line buffer; flush on newline or when
 * the buffer fills. The buffered bytes are written to the host UART with the
 * per-guest prefix so lines are attributed and never interleave mid-line. */
static void vuart_putc(vuart_t *u, char c) {
  if (c == '\n' || u->len >= VUART_LINE_MAX - 1) {
    vuart_prefix(u);
    for (uint32_t i = 0; i < u->len; i++) {
      uart_putc(u->line[i]);
    }
    u->len = 0;
    if (c == '\n') {
      uart_putc('\n');
      u->at_line_start = 1;
    }
    if (c != '\n') {
      /* buffer overflow flush: keep the char that triggered it */
      u->line[u->len++] = c;
    }
    return;
  }
  u->line[u->len++] = c;
}

void vuart_emulate(vuart_t *u, uint64_t ipa, int is_write, uint64_t *val,
                   int size_bytes) {
  (void)size_bytes;
  uint64_t off = ipa - VUART_BASE;

  if (is_write) {
    switch (off) {
    case R_DR:
      vuart_putc(u, (char)(*val & 0xFF));
      break;
    /* Init writes (CR/ICR/IBRD/FBRD/LCRH) and anything else: accept + drop. */
    default:
      break;
    }
    return;
  }

  /* Reads. */
  switch (off) {
  case R_FR:
    /* TX always ready (TXFF=0), nothing to receive (RXFE=1). This lets the
     * guest's TX poll loop proceed and its RX poll loop see an empty FIFO. */
    *val = FR_RXFE;
    break;
  case R_DR:
    *val = 0; /* no input wired yet */
    break;
  default:
    *val = 0;
    break;
  }
}

void vuart_flush(vuart_t *u) {
  if (u->len > 0) {
    vuart_prefix(u);
    for (uint32_t i = 0; i < u->len; i++) {
      uart_putc(u->line[i]);
    }
    u->len = 0;
    /* Leave at_line_start as-is: the line is not newline-terminated, so the
     * next flush of the same guest continues it without a new prefix. */
  }
}
