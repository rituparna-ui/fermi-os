#ifndef HYP_VUART_H
#define HYP_VUART_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Virtual PL011 UART for guest consoles.
 *
 * The guest's PL011 MMIO window (0x09000000) is left stage-2-UNMAPPED so every
 * access traps to EL2. We emulate the small register set FermiOS's uart.c uses
 * (DR/FR + the init writes CR/ICR/IBRD/FBRD/LCRH). Each guest gets its own
 * vuart_t whose TX is line-buffered and flushed to the REAL host UART with a
 * "[name] " prefix, so multiple guests' consoles are cleanly attributed and
 * never interleave mid-line. This also tightens isolation: no guest touches
 * the physical UART directly.
 * ------------------------------------------------------------------------- */

#define VUART_LINE_MAX 256
#define VUART_RX_MAX   64

typedef struct vuart {
  const char *name;            /* console tag, e.g. "vm0" */
  char        line[VUART_LINE_MAX];
  uint32_t    len;
  int         at_line_start;   /* whether the prefix is still pending */
  /* RX FIFO: bytes the hypervisor has routed to this guest's console. */
  uint8_t     rx[VUART_RX_MAX];
  uint32_t    rx_head;         /* next byte the guest will read */
  uint32_t    rx_tail;         /* next slot the host will fill   */
} vuart_t;

/* True if `ipa` falls in the emulated PL011 window. */
int vuart_is_target(uint64_t ipa);

/* Initialise a guest's virtual UART with a console tag. */
void vuart_init(vuart_t *u, const char *name);

/* Emulate a trapped PL011 access. is_write selects direction; *val is the
 * source (write) or destination (read); size_bytes is the access width. */
void vuart_emulate(vuart_t *u, uint64_t ipa, int is_write, uint64_t *val,
                   int size_bytes);

/* Flush any buffered partial line (called when a guest is descheduled so its
 * tail does not get attributed to the next guest). */
void vuart_flush(vuart_t *u);

/* Queue an input byte for the guest to read via its UART RX path. Dropped if
 * the RX FIFO is full. */
void vuart_rx_push(vuart_t *u, uint8_t c);

#endif /* HYP_VUART_H */
