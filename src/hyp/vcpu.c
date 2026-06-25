#include "vcpu.h"
#include "hyp.h"
#include "hyp_sysregs.h"
#include "vgic/vgic.h"
#include "timer/vtimer.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Multi-VM vCPU management + the EL2 round-robin scheduler.
 *
 * Two guests share the single physical CPU. The hypervisor world-switches
 * between them on its OWN timer tick (EL2 virtual timer CNTHV_EL2, PPI 28),
 * saving the outgoing guest's complete EL1/FP/vGIC state and restoring the
 * incoming guest's. Each guest has its own stage-2 (distinct VMID + VTTBR),
 * so the same guest IPA maps to different host PAs — true memory isolation.
 * ------------------------------------------------------------------------- */

#define MAX_VCPUS 2

static vcpu_t vcpus[MAX_VCPUS];
static int    nr_vcpus;
vcpu_t       *cur_vcpu;

/* Implemented in vcpu_switch.S — load gp->x[]/elr/spsr and eret (no return). */
extern void vcpu_first_entry(vcpu_gp_t *gp) __attribute__((noreturn));

/* Capture the current (QEMU reset) EL1 sysregs as a valid baseline both VMs
 * start from. Called once at boot before any guest runs. */
static void capture_reset_baseline(vcpu_sysregs_t *s) { vcpu_save_sysregs(s); }

vcpu_t *vcpu_alloc(const char *name, uint64_t entry_ipa, uint64_t vttbr,
                   uint64_t sp_el1_override) {
  vcpu_t *v = &vcpus[nr_vcpus];
  v->id = (uint32_t)nr_vcpus;
  v->vmid = (uint32_t)(nr_vcpus + 1); /* VMID 1, 2 (0 reserved) */
  v->name = name;
  v->vttbr_el2 = vttbr;
  v->runnable = 1;

  /* GP: enter the guest at its IPA in EL1h with DAIF masked (guest unmasks). */
  for (int i = 0; i < 31; i++) v->gp.x[i] = 0;
  v->gp.elr_el2 = entry_ipa;
  v->gp.spsr_el2 = SPSR_EL2_GUEST_ENTRY;

  /* Start from the captured reset sysreg baseline. */
  capture_reset_baseline(&v->sys);
  if (sp_el1_override) {
    v->sys.sp_el1 = sp_el1_override;
  }

  /* FP reset state = all zero (fpsr/fpcr 0). */
  for (unsigned i = 0; i < sizeof(v->fp.q); i++) v->fp.q[i] = 0;
  v->fp.fpsr = 0;
  v->fp.fpcr = 0;

  /* vGIC reset: virtual interface enabled, group1 + PMR seeded. The MMIO
   * model fields start at 0 (guest programs them). */
  vgic_vcpu_reset(&v->vgic);

  /* vtimer reset: disarmed. */
  v->vtimer.cval = 0;
  v->vtimer.ctl = 0;

  nr_vcpus++;
  return v;
}

/* Restore the full guest context for `v` into the hardware. */
static void vcpu_load(vcpu_t *v) {
  cur_vcpu = v;
  vcpu_restore_sysregs(&v->sys);
  vcpu_restore_fp(&v->fp);
  vgic_restore(&v->vgic);
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(v->vttbr_el2));
  /* Re-arm the EL2 physical timer (CNTHP) to this guest's shadow deadline. */
  vtimer_reprogram_current();
}

/* First-ever entry into vcpus[0]. Does not return. */
__attribute__((noreturn)) void vcpu_run_first(void) {
  cur_vcpu = &vcpus[0];
  vcpu_load(&vcpus[0]);
  vcpu_first_entry(&vcpus[0].gp);
}

/* --- EL2 scheduler tick via the EL2 PHYSICAL timer (CNTHP_EL2, PPI 26) -----
 * We reuse CNTHP — the same EL2 physical timer the vtimer uses — because its
 * IRQ delivery to EL2 is proven (the EL2 virtual timer CNTHV/PPI 28 does not
 * deliver reliably on this QEMU). The vtimer and the scheduler share CNTHP by
 * always arming it to the SOONER of {the running guest's vtimer deadline, the
 * next scheduler slice}. On each CNTHP fire the IRQ handler services whichever
 * deadline(s) elapsed. A coarse slice (~50 ms) keeps the round-robin visible. */
#define SCHED_SLICE_TICKS (62500000ULL / 20) /* ~50 ms at 62.5 MHz */

static uint64_t sched_deadline; /* absolute CNTPCT of the next scheduler slice */

/* Arm CNTHP to min(scheduler deadline, current guest's vtimer deadline). */
void hyp_cnthp_arm(void) {
  uint64_t deadline = sched_deadline;
  /* Fold in the running guest's vtimer deadline if armed + sooner. */
  if (cur_vcpu && (cur_vcpu->vtimer.ctl & 1ULL) /*ENABLE*/) {
    uint64_t vt = cur_vcpu->vtimer.cval;
    if (vt < deadline) deadline = vt;
  }
  __asm__ __volatile__("msr cnthp_cval_el2, %0" ::"r"(deadline));
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(1ULL));
}

uint64_t hyp_sched_deadline(void) { return sched_deadline; }

static void sched_arm_slice(void) {
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
  sched_deadline = now + SCHED_SLICE_TICKS;
  hyp_cnthp_arm();
}

void vcpu_sched_init(void) {
  sched_arm_slice();
  hyp_puts("[SCHED] EL2 scheduler armed (CNTHP PPI 26, ~50ms slice)\n");
}

/* Pick the next runnable vCPU round-robin after `cur`. */
static vcpu_t *pick_next(vcpu_t *cur) {
  for (int i = 1; i <= nr_vcpus; i++) {
    vcpu_t *cand = &vcpus[(cur->id + i) % nr_vcpus];
    if (cand->runnable) return cand;
  }
  return cur;
}

/* Scheduler tick: called from the EL2 IRQ handler when CNTHV (PPI 28) fires.
 * Saves the running guest's context out of the trap frame, restores the next
 * guest's into the frame, so the vector exit path erets into the new guest. */
void vcpu_sched_tick(hyp_trap_frame_t *f) {
  sched_arm_slice(); /* schedule the next slice (also re-arms CNTHP) */

  vcpu_t *prev = cur_vcpu;
  vcpu_t *next = pick_next(prev);
  if (next == prev) {
    return; /* only one runnable guest — keep running it */
  }
  /* Log every 100th switch so the round-robin is visible without flooding. */
  static uint64_t nswitch;
  if ((nswitch++ % 100) == 0) {
    hyp_puts("[SCHED] world switches: ");
    hyp_puthex(nswitch - 1);
    hyp_putc('\n');
  }

  /* Save prev: GP from frame, then EL1/FP/vGIC from hardware. */
  for (int i = 0; i < 31; i++) prev->gp.x[i] = f->regs[i];
  prev->gp.elr_el2 = f->elr;
  prev->gp.spsr_el2 = f->spsr;
  vcpu_save_sysregs(&prev->sys);
  vcpu_save_fp(&prev->fp);
  vgic_save(&prev->vgic);

  /* Restore next into hardware. */
  vcpu_load(next);

  /* Load next's GP into the frame so the vector exit erets into it. */
  for (int i = 0; i < 31; i++) f->regs[i] = next->gp.x[i];
  f->elr = next->gp.elr_el2;
  f->spsr = next->gp.spsr_el2;
}
