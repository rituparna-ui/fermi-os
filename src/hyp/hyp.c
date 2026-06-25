#include "hyp.h"
#include "hyp_sysregs.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EL2 hypervisor C core (Milestone 1).
 *
 * Everything here runs at EL2 with the MMU OFF, so all addresses are physical.
 * We cannot reuse the guest's uart.c: it is part of the guest image and its
 * symbols live at guest VAs. The PL011 UART is at the same physical address
 * (0x09000000) for both, so a tiny self-contained poke is all we need. QEMU
 * has already initialised the PL011 enough for output at reset, and the guest
 * re-initialises it anyway, so we only touch the data register.
 * ------------------------------------------------------------------------- */

#define HYP_UART_BASE 0x09000000UL
#define HYP_UART_DR   (HYP_UART_BASE + 0x00)
#define HYP_UART_FR   (HYP_UART_BASE + 0x18)
#define HYP_UART_FR_TXFF (1U << 5) /* transmit FIFO full */

static inline void mmio_w32(uint64_t addr, uint32_t val) {
  *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_r32(uint64_t addr) {
  return *(volatile uint32_t *)addr;
}

void hyp_putc(char c) {
  /* Spin while the TX FIFO is full, then push the byte. */
  while (mmio_r32(HYP_UART_FR) & HYP_UART_FR_TXFF) {
  }
  mmio_w32(HYP_UART_DR, (uint32_t)(unsigned char)c);
}

void hyp_puts(const char *s) {
  for (; *s; s++) {
    if (*s == '\n') {
      hyp_putc('\r');
    }
    hyp_putc(*s);
  }
}

void hyp_puthex(uint64_t v) {
  static const char digits[] = "0123456789ABCDEF";
  hyp_putc('0');
  hyp_putc('x');
  for (int shift = 60; shift >= 0; shift -= 4) {
    hyp_putc(digits[(v >> shift) & 0xF]);
  }
}

/* Called from the EL2 vector stubs (hyp_vectors.S). At Milestone 1 no vector
 * should ever fire, so reaching here means something unexpected trapped to
 * EL2. Report the index + syndrome and halt. Does not return. */
__attribute__((noreturn)) void hyp_vector_report(uint64_t index, uint64_t esr,
                                                 uint64_t elr, uint64_t far) {
  hyp_puts("\n[HYP][TRAP] unexpected EL2 exception, vector index = ");
  hyp_puthex(index);
  hyp_puts("\n  ESR_EL2 = ");
  hyp_puthex(esr);
  hyp_puts("\n  ELR_EL2 = ");
  hyp_puthex(elr);
  hyp_puts("\n  FAR_EL2 = ");
  hyp_puthex(far);
  hyp_putc('\n');
  hyp_panic("unexpected EL2 trap (M1: nothing should trap to EL2)");
}

__attribute__((noreturn)) void hyp_panic(const char *msg) {
  hyp_puts("\n[HYP][PANIC] ");
  if (msg) {
    hyp_puts(msg);
  }
  hyp_putc('\n');
  for (;;) {
    __asm__ __volatile__("wfe");
  }
}

/* Called from hyp_boot.S after the EL2 register context is established and
 * just before the eret into the guest. For Milestone 1 there is no stage-2,
 * no vGIC and no vtimer: the guest image has already been placed at physical
 * 0x40000000 by QEMU '-device loader', and the eret will land on its _start.
 *
 * We read back a couple of registers purely as a sanity announcement so the
 * serial log makes the EL2 -> EL1 transition observable. */
void hyp_main(void) {
  uint64_t current_el, hcr, cptr;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(current_el));
  __asm__ __volatile__("mrs %0, hcr_el2" : "=r"(hcr));
  __asm__ __volatile__("mrs %0, cptr_el2" : "=r"(cptr));

  hyp_puts("\n");
  hyp_puts("==================================================\n");
  hyp_puts("  Fermi Hypervisor (EL2) - Milestone 1\n");
  hyp_puts("==================================================\n");
  hyp_puts("[HYP] CurrentEL = ");
  hyp_puthex(current_el);
  hyp_puts("  (expect 0x8 = EL2)\n");
  hyp_puts("[HYP] HCR_EL2   = ");
  hyp_puthex(hcr);
  hyp_puts("  (RW=1, VM=0 passthrough)\n");
  hyp_puts("[HYP] CPTR_EL2  = ");
  hyp_puthex(cptr);
  hyp_puts("  (FP/SVE not trapped)\n");
  hyp_puts("[HYP] guest @ IPA ");
  hyp_puthex(GUEST_ENTRY_IPA);
  hyp_puts(" -> eret to EL1...\n");
  hyp_puts("--------------------------------------------------\n\n");
}
