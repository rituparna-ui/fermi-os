#include "vtimer.h"
#include "vgic/vgic.h"
#include "vcpu.h"
#include "hyp.h"
#include <stdint.h>

/* CNTHP is shared with the scheduler; arm it via the scheduler's min-deadline
 * helper rather than programming the comparator directly. */
extern void hyp_cnthp_arm(void);

/* ---------------------------------------------------------------------------
 * Virtual EL1 physical timer via trap-and-emulate, backed by the EL2 physical
 * timer (CNTHP_*_EL2). See vtimer.h for the rationale (level-triggered storm).
 *
 * The guest's shadow CNTP_CTL/CVAL lives PER-vCPU in cur_vcpu->vtimer, so each
 * guest has its own timeline. On a world switch the scheduler reprograms CNTHP
 * from the incoming guest's shadow (vtimer_reprogram_current).
 * ------------------------------------------------------------------------- */

#define CNT_CTL_ENABLE  (1ULL << 0)
#define CNT_CTL_IMASK   (1ULL << 1)
#define CNT_CTL_ISTATUS (1ULL << 2)

void vtimer_init(void) {
  /* Trap guest CNTP_* (EL1PCEN=0) but allow guest CNTPCT/CNTFRQ reads
   * (EL1PCTEN=1). Overrides the M1/M2 passthrough value 0x3. */
  uint64_t cnthctl = CNT_CTL_ENABLE; /* bit0 EL1PCTEN=1; bit1 EL1PCEN=0 */
  __asm__ __volatile__("msr cnthctl_el2, %0\n\tisb" ::"r"(cnthctl));
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(0ULL));

  hyp_puts("[VTIMER] CNTHCTL_EL2=");
  hyp_puthex(cnthctl);
  hyp_puts(" (trap CNTP_*, allow CNTPCT). EL2 timer disarmed.\n");
}

/* Re-arm the shared EL2 physical timer from the CURRENT guest's shadow state.
 * CNTHP is multiplexed with the scheduler slice, so we delegate to
 * hyp_cnthp_arm() which programs it to min(scheduler, this guest's vtimer). */
void vtimer_reprogram_current(void) { hyp_cnthp_arm(); }

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

  vcpu_vtimer_t *vt = &cur_vcpu->vtimer;

  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  switch (op2) {
  case 0: /* CNTP_TVAL_EL0 (signed 32-bit = CVAL - now) */
    if (is_read) {
      /* Saturate to the signed 32-bit range rather than wrapping: a deadline
       * far in the future must not read back as a (negative) "already fired". */
      int64_t delta = (int64_t)(vt->cval - now);
      int32_t tval_s32 = (delta > 0x7FFFFFFFLL)    ? 0x7FFFFFFF
                         : (delta < -(1LL << 31))  ? (int32_t)(-(1LL << 31))
                                                   : (int32_t)delta;
      if (rt != 31) f->regs[rt] = (uint64_t)(uint32_t)tval_s32;
    } else {
      uint64_t tval = (rt == 31) ? 0 : f->regs[rt];
      int32_t s = (int32_t)(uint32_t)tval;
      vt->cval = now + (uint64_t)(int64_t)s;
      vt->pending = 0; /* re-arming clears the latched condition */
      vtimer_reprogram_current();
    }
    return 1;

  case 1: /* CNTP_CTL_EL0 */
    if (is_read) {
      uint64_t ctl = vt->ctl & (CNT_CTL_ENABLE | CNT_CTL_IMASK);
      /* ISTATUS reflects the latched condition (set when CNTHP fired and the
       * vIRQ was injected) OR the live comparator. The guest's IRQ handler
       * reads CNTP_CTL to confirm the timer fired — it must see ISTATUS=1. */
      if (vt->pending ||
          ((vt->ctl & CNT_CTL_ENABLE) && now >= vt->cval)) {
        ctl |= CNT_CTL_ISTATUS;
      }
      if (rt != 31) f->regs[rt] = ctl;
    } else {
      vt->ctl = (rt == 31) ? 0 : (f->regs[rt] & (CNT_CTL_ENABLE | CNT_CTL_IMASK));
      vtimer_reprogram_current();
    }
    return 1;

  case 2: /* CNTP_CVAL_EL0 (absolute deadline) */
    if (is_read) {
      if (rt != 31) f->regs[rt] = vt->cval;
    } else {
      vt->cval = (rt == 31) ? 0 : f->regs[rt];
      vt->pending = 0; /* re-arming clears the latched condition */
      vtimer_reprogram_current();
    }
    return 1;
  }
  return 0;
}

/* Timer-deadline handling (latch pending + inject vINTID 30) now lives in
 * vcpu_wake_expired() in vcpu.c, which handles non-current vCPUs too. */
