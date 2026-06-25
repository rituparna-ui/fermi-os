#include "hyp.h"
#include "exception.h" /* ESR_EC, EC_HVC_AARCH64, ESR_ISS_* */
#include "uart/uart.h"

/* FermiOS EL2 hypervisor — milestone 2: EL2 trap plumbing + self-test.
 *
 * Under VHE the host installed its own exception table via the redirected
 * VBAR_EL1 (== VBAR_EL2). This module owns a SEPARATE table (hyp_vector_table)
 * that reads the EL2-banked syndrome registers and will become the guest-exit
 * path in milestone 3. Here we prove it works end-to-end without a guest: swap
 * it into VBAR_EL2, execute an HVC from the host (EL2), confirm the exception
 * is captured with the right syndrome, then restore the host table. */

int hyp_at_el2(void) {
  uint64_t el;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(el));
  return (int)(((el >> 2) & 3) == 2);
}

/* Self-test handshake: set by hyp_dispatch when the deliberate HVC lands. */
static volatile int hyp_selftest_hit;
static volatile uint64_t hyp_selftest_esr;

void hyp_dispatch(uint64_t type, hyp_trap_frame_t *frame) {
  uint64_t ec = ESR_EC(frame->esr);

  if (type == HYP_EXC_SYNC && ec == EC_HVC_AARCH64) {
    /* Hypercall. ELR_EL2 already points at the instruction after the HVC,
     * so there is nothing to advance — just record and return. The real
     * hypercall dispatcher arrives in a later milestone. */
    hyp_selftest_hit = 1;
    hyp_selftest_esr = frame->esr;
    return;
  }

  /* Anything else reaching the EL2 table right now is unexpected (the host
   * has its own table; this one is only live around the self-test and, later,
   * while a guest runs). Decode loudly and park — returning could fault-loop. */
  uart_println("");
  uart_println("===== UNEXPECTED EL2 TRAP =====");
  uart_printf("  type=%u  ESR_EL2=%x  EC=%x\n", type, frame->esr, ec);
  uart_printf("  ELR_EL2=%x  FAR_EL2=%x  HPFAR_EL2=%x\n",
              frame->elr, frame->far, frame->hpfar);
  uart_println("  (hypervisor bug — parking)");
  uart_println("===============================");
  for (;;) {
    __asm__ __volatile__("wfi");
  }
}

void hyp_init(void) {
  if (!hyp_at_el2()) {
    uart_println("[HYP] not at EL2 (legacy EL1 boot) — hypervisor layer disabled");
    return;
  }

  uart_println("[HYP] EL2 VHE host detected — exercising dedicated EL2 vector table");

  /* The host installed its table through the VHE-redirected vbar_el1, so the
   * live VBAR_EL2 IS the host table — save it to restore afterwards. */
  uint64_t host_vbar;
  __asm__ __volatile__("mrs %0, vbar_el2" : "=r"(host_vbar));

  /* Mask IRQ + FIQ so the only exception that can reach our temporary table
   * is the synchronous HVC we are about to issue. */
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #3");

  uint64_t hyp_vbar = (uint64_t)(uintptr_t)hyp_vector_table;
  __asm__ __volatile__("msr vbar_el2, %0\n\tisb" ::"r"(hyp_vbar));

  hyp_selftest_hit = 0;
  /* HVC at EL2 is taken to EL2 via the Current-EL/SP_ELx synchronous vector. */
  __asm__ __volatile__("hvc #0" ::: "memory");

  /* Restore the host table and DAIF before doing anything else. */
  __asm__ __volatile__("msr vbar_el2, %0\n\tisb" ::"r"(host_vbar));
  __asm__ __volatile__("msr daif, %0" ::"r"(daif));

  if (hyp_selftest_hit) {
    uart_printf("[HYP] self-test OK: HVC routed to EL2 table, ESR_EL2=%x EC=%x (HVC)\n",
                hyp_selftest_esr, ESR_EC(hyp_selftest_esr));
  } else {
    uart_println("[HYP] self-test FAILED: HVC did not reach the EL2 table");
  }
  uart_printf("[HYP] host VBAR_EL2 restored (%x); EL2 trap plumbing live\n",
              host_vbar);
}
