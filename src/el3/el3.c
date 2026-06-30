#include "el3.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * el3.c — EL3 Root-world Secure Monitor (E0a)
 *
 * Runs at EL3 with the MMU off. For now it just brings up the console and
 * reports the world state before boot.S ERETs to the Non-secure world. The
 * GPT/GPC setup (E0b) and the Realm-world launch (E1) build on this.
 *
 * EL3 owns SCR_EL3 (world selection / SMC routing) and, with FEAT_RME, the
 * Granule Protection Check registers GPCCR_EL3 / GPTBR_EL3. We read a few here
 * purely to show they are reachable from Root world.
 * --------------------------------------------------------------------------- */

/* Dedicated EL3 stack (monitor-private; lives in the reserved .hyp_tables
 * region so it is neither visible to the Non-secure world's stage-2 nor
 * reused by the guest PMM). Size must match EL3_STACK_SIZE in boot.S. */
__attribute__((aligned(16), section(".hyp_tables"))) uint8_t el3_stack[8192];

#define MRS(reg)                                                               \
  ({                                                                           \
    uint64_t _v;                                                               \
    __asm__ __volatile__("mrs %0, " #reg : "=r"(_v));                          \
    _v;                                                                        \
  })

/* PL011 UART0 on QEMU virt. With secure=on this UART is reachable from Root. */
#define UART0 0x09000000UL
#define U_DR (UART0 + 0x00)
#define U_FR (UART0 + 0x18)
#define U_IBRD (UART0 + 0x24)
#define U_FBRD (UART0 + 0x28)
#define U_LCRH (UART0 + 0x2C)
#define U_CR (UART0 + 0x30)
#define U_ICR (UART0 + 0x44)
#define U_FR_TXFF (1u << 5)

static inline void w32(uint64_t a, uint32_t v) {
  *(volatile uint32_t *)a = v;
}
static inline uint32_t r32(uint64_t a) {
  return *(volatile uint32_t *)a;
}

/* QEMU 11's PL011 drops writes while the UART is disabled, so the monitor must
 * enable it before printing (older QEMU is lenient, but we do it always). */
static void el3_uart_init(void) {
  w32(U_CR, 0);
  w32(U_ICR, 0x7FF);
  w32(U_IBRD, 13);
  w32(U_FBRD, 2);
  w32(U_LCRH, (1 << 4) | (1 << 5) | (1 << 6));
  w32(U_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

static void el3_putc(char c) {
  while (r32(U_FR) & U_FR_TXFF) {
  }
  w32(U_DR, (uint32_t)c);
}

static void el3_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      el3_putc('\r');
    el3_putc(*s++);
  }
}

static void el3_puthex(uint64_t v) {
  el3_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uint64_t nib = (v >> i) & 0xF;
    el3_putc((char)(nib < 10 ? '0' + nib : 'a' + (nib - 10)));
  }
}

void el3_init(void) {
  el3_uart_init();
  el3_puts("\n[EL3] Root-world Secure Monitor online\n");

  uint64_t el = (MRS(CurrentEL) >> 2) & 0x3;
  el3_puts("[EL3] CurrentEL = ");
  el3_puthex(el);
  el3_puts(" (Root world)\n");

  /* Prove the RME Granule-Protection registers are reachable from Root.
   * GPCCR_EL3 = S3_6_C2_C1_6, GPTBR_EL3 = S3_6_C2_C1_4. */
  uint64_t gpccr = MRS(S3_6_C2_C1_6);
  el3_puts("[EL3] GPCCR_EL3 = ");
  el3_puthex(gpccr);
  el3_puts(" (GPC not yet enabled)\n");

  el3_puts("[EL3] dropping to Non-secure EL2 (Fermi host)...\n");
}
