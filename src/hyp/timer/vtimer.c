#include "vtimer.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Virtual EL1 physical timer via trap-and-emulate, backed by the EL2 physical
 * timer (CNTHP_*_EL2). See vtimer.h for the rationale (level-triggered storm).
 *
 * Shadow of the guest's CNTP_* registers. The guest writes CNTP_CVAL_EL0
 * (absolute deadline) and CNTP_CTL_EL0 (enable/imask); both trap here. We mirror
 * them into CNTHP_CVAL_EL2 / CNTHP_CTL_EL2 so the EL2 comparator fires at the
 * guest's deadline and the resulting PPI 26 is taken at EL2, where we inject
 * vINTID 30.
 * ------------------------------------------------------------------------- */

/* CNTP_CTL bit fields (shared by CNTP_CTL_EL0 and CNTHP_CTL_EL2). */
#define CNT_CTL_ENABLE  (1ULL << 0)
#define CNT_CTL_IMASK   (1ULL << 1)
#define CNT_CTL_ISTATUS (1ULL << 2)

static uint64_t guest_cval; /* shadow CNTP_CVAL_EL0 */
static uint64_t guest_ctl;  /* shadow CNTP_CTL_EL0  */

void vtimer_init(void) {
  /* Trap guest CNTP_* (EL1PCEN=0) but allow guest CNTPCT/CNTFRQ reads
   * (EL1PCTEN=1). This overrides the M1/M2 passthrough value 0x3. */
  uint64_t cnthctl = CNT_CTL_ENABLE; /* bit0 EL1PCTEN=1; bit1 EL1PCEN=0 */
  __asm__ __volatile__("msr cnthctl_el2, %0\n\tisb" ::"r"(cnthctl));

  /* Make sure our EL2 timer starts disarmed. */
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(0ULL));

  hyp_puts("[VTIMER] CNTHCTL_EL2=");
  hyp_puthex(cnthctl);
  hyp_puts(" (trap CNTP_*, allow CNTPCT). EL2 timer disarmed.\n");
}

/* Re-arm or disarm the EL2 physical timer from the guest's shadow state. */
static void vtimer_reprogram(void) {
  if ((guest_ctl & CNT_CTL_ENABLE) && !(guest_ctl & CNT_CTL_IMASK)) {
    __asm__ __volatile__("msr cnthp_cval_el2, %0" ::"r"(guest_cval));
    __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(CNT_CTL_ENABLE));
  } else {
    __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(0ULL));
  }
}

/* Decode a trapped CNTP_* access. The generic-timer EL0 registers are in
 * CRn=14: CNTP_TVAL_EL0 = (op0=3,op1=3,crn=14,crm=2,op2=0),
 *         CNTP_CTL_EL0  = (.. crm=2, op2=1),
 *         CNTP_CVAL_EL0 = (.. crm=2, op2=2). We match on CRn=14, CRm=2. */
int vtimer_emulate_sysreg(hyp_trap_frame_t *f) {
  uint64_t esr = f->esr;
  uint32_t op0 = (uint32_t)((esr >> 20) & 0x3);
  uint32_t crn = (uint32_t)((esr >> 10) & 0xF);
  uint32_t crm = (uint32_t)((esr >> 1) & 0xF);
  uint32_t op2 = (uint32_t)((esr >> 17) & 0x7);
  uint32_t rt  = (uint32_t)((esr >> 5) & 0x1F);
  uint32_t is_read = (uint32_t)(esr & 0x1);

  if (op0 != 3 || crn != 14 || crm != 2) {
    return 0; /* not a CNTP_* timer register */
  }

  /* Current physical count, used for TVAL (relative) <-> CVAL (absolute). */
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  switch (op2) {
  case 0: /* CNTP_TVAL_EL0 (signed 32-bit timer value = CVAL - now) */
    if (is_read) {
      uint64_t tval = (guest_cval - now) & 0xFFFFFFFFULL;
      if (rt != 31) f->regs[rt] = tval;
    } else {
      uint64_t tval = (rt == 31) ? 0 : f->regs[rt];
      int32_t s = (int32_t)(uint32_t)tval;
      guest_cval = now + (uint64_t)(int64_t)s;
      vtimer_reprogram();
    }
    return 1;

  case 1: /* CNTP_CTL_EL0 */
    if (is_read) {
      /* Recompute ISTATUS from the live count so the guest sees a truthful
       * condition-met bit. */
      uint64_t ctl = guest_ctl & (CNT_CTL_ENABLE | CNT_CTL_IMASK);
      if ((guest_ctl & CNT_CTL_ENABLE) && now >= guest_cval) {
        ctl |= CNT_CTL_ISTATUS;
      }
      if (rt != 31) f->regs[rt] = ctl;
    } else {
      guest_ctl = (rt == 31) ? 0 : (f->regs[rt] & (CNT_CTL_ENABLE | CNT_CTL_IMASK));
      vtimer_reprogram();
    }
    return 1;

  case 2: /* CNTP_CVAL_EL0 (absolute deadline) */
    if (is_read) {
      if (rt != 31) f->regs[rt] = guest_cval;
    } else {
      guest_cval = (rt == 31) ? 0 : f->regs[rt];
      vtimer_reprogram();
    }
    return 1;
  }
  return 0;
}

void vtimer_handle_host_irq(void) {
  /* The EL2 comparator fired. Disarm it (level-triggered: leaving it enabled
   * would re-fire immediately), then inject the guest's virtual timer IRQ.
   * The guest will service it and re-arm CNTP_CVAL_EL0, which traps back to
   * vtimer_emulate_sysreg -> vtimer_reprogram and re-arms CNTHP. */
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(0ULL));
  vgic_inject_ppi(VTIMER_GUEST_PPI);
}
