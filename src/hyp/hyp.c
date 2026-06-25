#include "hyp.h"
#include "hyp_alloc.h"
#include "hyp_sysregs.h"
#include "stage2.h"
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

/* Guest flat image embedded in the hyp (see guest_blob.S). */
extern const uint8_t __guest_blob_start[];
extern const uint8_t __guest_blob_end[];

/* Copy the embedded guest image to its physical load base (0x40000000) so the
 * eret lands on real guest bytes. Done with the EL2 MMU off (physical stores).
 * The guest's first LOAD segment starts at PA 0x40000000 and the flat blob
 * preserves inter-segment gaps, so one linear copy places every PT_LOAD. */
static void hyp_load_guest(void) {
  const uint8_t *src = __guest_blob_start;
  uint64_t size = (uint64_t)(__guest_blob_end - __guest_blob_start);
  volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)GUEST_ENTRY_IPA;

  /* Word copy for the aligned bulk, byte copy for the tail. */
  uint64_t words = size / 8;
  const uint64_t *s64 = (const uint64_t *)src;
  volatile uint64_t *d64 = (volatile uint64_t *)dst;
  for (uint64_t i = 0; i < words; i++) {
    d64[i] = s64[i];
  }
  for (uint64_t i = words * 8; i < size; i++) {
    dst[i] = src[i];
  }
  /* Ensure the stores are visible and instruction-coherent before the guest
   * (at EL1) fetches them: clean D-cache then invalidate I-cache to PoU. */
  hyp_dcache_clean_range(GUEST_ENTRY_IPA, size);
  __asm__ __volatile__("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

  hyp_puts("[HYP] guest image copied to ");
  hyp_puthex(GUEST_ENTRY_IPA);
  hyp_puts(" (");
  hyp_puthex(size);
  hyp_puts(" bytes)\n");
}

/* Called from hyp_boot.S after the EL2 register context is established and
 * just before the eret into the guest. Places the guest image, builds stage-2,
 * enables HCR_EL2.VM, and returns; hyp_boot.S then erets into the guest. */
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

  /* Place the embedded guest image at its physical load base. */
  hyp_load_guest();

  /* Milestone 2: build the stage-2 identity tables and program VTCR/VTTBR,
   * then enable stage-2 by setting HCR_EL2.VM=1. From this point the guest
   * runs in a real VM address space: every stage-1 output (IPA) is walked
   * through our stage-2 tables before reaching host PA. */
  s2_init();

  uint64_t hcr2 = HCR_EL2_M2;
  __asm__ __volatile__("msr hcr_el2, %0\n\tisb" ::"r"(hcr2));
  __asm__ __volatile__("mrs %0, hcr_el2" : "=r"(hcr));
  hyp_puts("[HYP] HCR_EL2   = ");
  hyp_puthex(hcr);
  hyp_puts("  (VM=1, stage-2 ON)\n");

  hyp_puts("[HYP] guest @ IPA ");
  hyp_puthex(GUEST_ENTRY_IPA);
  hyp_puts(" -> eret to EL1 (now translated)...\n");
  hyp_puts("--------------------------------------------------\n\n");
}
