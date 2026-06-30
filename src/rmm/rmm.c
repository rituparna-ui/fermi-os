#include "el3/el3.h" /* RMM_BOOT_COMPLETE */
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * rmm.c — Realm Management Monitor, Realm-EL2 entry (E1 stub)
 *
 * Launched by the EL3 monitor with an ERET into the Realm world
 * (SCR_EL3.{NSE,NS} = 0b11) at Realm-EL2, MMU off. For this milestone it only
 * proves the world switch: it announces itself and hands control back to EL3
 * via SMC(RMM_BOOT_COMPLETE). Later milestones grow this into the full RMM
 * (RMI/RSI, granule/realm/REC, attestation) running in the Realm world.
 *
 * Runs on its own stack (rmm_stack) in the reserved .hyp_tables region. The
 * UART was already enabled by the EL3 monitor; with the all-access GPT the
 * Realm world can reach it.
 * --------------------------------------------------------------------------- */

__attribute__((aligned(16), section(".hyp_tables"))) uint8_t rmm_stack[8192];

#define U_DR 0x09000000UL
#define U_FR 0x09000018UL
#define U_FR_TXFF (1u << 5)

static void rmm_putc(char c) {
  while (*(volatile uint32_t *)U_FR & U_FR_TXFF) {
  }
  *(volatile uint32_t *)U_DR = (uint32_t)c;
}

static void rmm_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      rmm_putc('\r');
    rmm_putc(*s++);
  }
}

void rmm_realm_main(void) {
  rmm_puts("\n[RMM] Realm-world monitor online at Realm-EL2\n");

  uint64_t el;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(el));
  rmm_puts("[RMM] CurrentEL = ");
  rmm_putc((char)('0' + ((el >> 2) & 3)));
  rmm_puts(" (Realm world)\n");

  rmm_puts("[RMM] boot complete; returning to EL3 via SMC\n");
  register uint64_t x0 __asm__("x0") = RMM_BOOT_COMPLETE;
  __asm__ __volatile__("smc #0" : "+r"(x0)::"memory");

  for (;;)
    __asm__ __volatile__("wfi");
}
