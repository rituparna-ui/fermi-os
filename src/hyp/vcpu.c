#include "vcpu.h"
#include "hyp.h"
#include "hyp_sysregs.h"
#include "stage2.h"
#include "snapshot.h"
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

#define MAX_VCPUS 6

static vcpu_t vcpus[MAX_VCPUS];
static int    nr_vcpus;
vcpu_t       *cur_vcpu;

/* Implemented in vcpu_switch.S — load gp->x[]/elr/spsr and eret (no return). */
extern void vcpu_first_entry(vcpu_gp_t *gp) __attribute__((noreturn));

/* Forward decls (definitions appear later in this file). */
static vcpu_t *pick_next(vcpu_t *cur);
static void switch_to(vcpu_t *next, hyp_trap_frame_t *f);
static void vcpu_load(vcpu_t *v);

/* Capture the current (QEMU reset) EL1 sysregs as a valid baseline both VMs
 * start from. Called once at boot before any guest runs. */
static void capture_reset_baseline(vcpu_sysregs_t *s) { vcpu_save_sysregs(s); }

/* (Re)initialise the resettable execution state of a vCPU to its pristine
 * boot values: GP/PC/PSTATE, EL1 sysregs (from the captured QEMU-reset
 * baseline), FP, vGIC, vtimer. Shared by vcpu_alloc and vcpu_reset. */
static void vcpu_init_state(vcpu_t *v) {
  /* GP: enter the guest at its IPA in EL1h with DAIF masked (guest unmasks).
   * x0 carries an optional role/arg (IPC producer vs consumer); the other
   * guests leave x0_init = 0, matching a clean reset. */
  for (int i = 0; i < 31; i++) v->gp.x[i] = 0;
  v->gp.x[0] = v->x0_init;
  v->gp.elr_el2 = v->entry_ipa;
  v->gp.spsr_el2 = SPSR_EL2_GUEST_ENTRY;

  /* Start from the captured reset sysreg baseline. */
  capture_reset_baseline(&v->sys);
  if (v->sp_el1_init) {
    v->sys.sp_el1 = v->sp_el1_init;
  }

  /* FP reset state = all zero. */
  for (unsigned i = 0; i < sizeof(v->fp.q); i++) v->fp.q[i] = 0;
  v->fp.fpsr = 0;
  v->fp.fpcr = 0;

  /* vGIC reset: virtual interface enabled, group1 + PMR seeded. */
  vgic_vcpu_reset(&v->vgic);

  /* vtimer reset: disarmed. */
  v->vtimer.cval = 0;
  v->vtimer.ctl = 0;
  v->vtimer.pending = 0;

  v->runnable = 1;
  v->dead = 0;
}

int vcpu_is_dead(const vcpu_t *v) { return v->dead; }

void vcpu_poweroff_current(hyp_trap_frame_t *f) {
  cur_vcpu->dead = 1;
  cur_vcpu->runnable = 0;
  vcpu_t *next = pick_next(cur_vcpu);
  if (next == cur_vcpu) {
    hyp_panic("last VM powered off — nothing left to run");
  }
  switch_to(next, f);
}

vcpu_t *vcpu_by_id(int id) {
  return (id >= 0 && id < nr_vcpus) ? &vcpus[id] : (vcpu_t *)0;
}

int vcpu_count(void) { return nr_vcpus; }

uint64_t vcpu_ipa_to_pa(const vcpu_t *v, uint64_t ipa, uint64_t len) {
  uint64_t base = v->entry_ipa;
  uint64_t end = base + v->ram_size;
  if (ipa < base || ipa >= end || len == 0 || ipa + len > end) {
    return 0; /* outside this VM's private RAM window */
  }
  return v->img_dst_pa + (ipa - base);
}

#define PV_LOG_MAX 256

int64_t vcpu_pv_log(uint64_t buf_ipa, uint64_t len) {
  if (len > PV_LOG_MAX) {
    len = PV_LOG_MAX;
  }
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, buf_ipa, len);
  if (!pa) {
    return -1; /* bad guest pointer — reject (do not read arbitrary host PA) */
  }
  /* Tag with the VM name so multiplexed guest logs are attributable. */
  hyp_puts("[");
  hyp_puts(cur_vcpu->name);
  hyp_puts("] ");
  const char *p = (const char *)(uintptr_t)pa;
  for (uint64_t i = 0; i < len; i++) {
    hyp_putc(p[i]);
  }
  return (int64_t)len;
}

int64_t vcpu_vmctl(uint64_t op, uint64_t target, uint64_t arg,
                   hyp_trap_frame_t *f) {
  /* Only the designated control domain may manage other VMs. */
  if (!cur_vcpu->privileged) {
    return VMCTL_EPERM;
  }
  if (op == VMCTL_COUNT) {
    return (int64_t)nr_vcpus;
  }

  vcpu_t *t = vcpu_by_id((int)target);
  if (!t) {
    return VMCTL_EINVAL;
  }
  switch (op) {
  case VMCTL_STATE: {
    int64_t st = 0;
    if (t->runnable) st |= VMCTL_ST_RUNNABLE;
    if (t->dead)     st |= VMCTL_ST_DEAD;
    st |= ((int64_t)t->vmid & 0xFF) << 8;
    return st;
  }
  case VMCTL_RUNS:
    return (int64_t)t->run_count;
  case VMCTL_STAT:
    switch (arg) {
    case VMSTAT_HVC:        return (int64_t)t->stats.hvc;
    case VMSTAT_DATA_ABORT: return (int64_t)t->stats.data_abort;
    case VMSTAT_SYSREG:     return (int64_t)t->stats.sysreg;
    case VMSTAT_WFX:        return (int64_t)t->stats.wfx;
    case VMSTAT_IRQ:        return (int64_t)t->stats.irq;
    default:                return VMCTL_EINVAL;
    }
  case VMCTL_WEIGHT:
    if (arg < 1) return VMCTL_EINVAL;
    t->weight = (uint32_t)arg;
    return VMCTL_OK;
  case VMCTL_CPUTIME:
    return (int64_t)t->cpu_ticks;
  case VMCTL_SNAPSHOT:
    return snapshot_save((int)target);
  case VMCTL_RESTORE:
    return snapshot_restore((int)target);
  case VMCTL_MIGRATE:
    return snapshot_clone((int)target);
  case VMCTL_RESET:
    if (t->dead) return VMCTL_EINVAL;
    /* Reset is in-place if it targets the caller; here the control VM never
     * resets itself, so t != cur_vcpu and f is unused for the target. */
    vcpu_reset(t, (t == cur_vcpu) ? f : (hyp_trap_frame_t *)0);
    return VMCTL_OK;
  case VMCTL_STOP:
    if (t == cur_vcpu) return VMCTL_EINVAL; /* don't pause the controller */
    t->paused = 1;
    return VMCTL_OK;
  case VMCTL_START:
    if (t->dead) return VMCTL_EINVAL;
    t->paused = 0;
    t->runnable = 1;
    return VMCTL_OK;
  default:
    return VMCTL_EINVAL;
  }
}

int vcpu_ring_doorbell(vcpu_t *from) {
  if (from->doorbell_target < 0) {
    return -1;
  }
  vcpu_t *peer = vcpu_by_id(from->doorbell_target);
  if (!peer || peer->dead) {
    return -1;
  }
  if (peer == cur_vcpu) {
    vgic_inject_ppi(DOORBELL_INTID);      /* live LR (peer is running) */
  } else {
    vgic_inject_to(&peer->vgic, DOORBELL_INTID); /* saved LR, presented on entry */
  }
  peer->runnable = 1; /* wake it if it was blocked on WFI */
  return 0;
}

vcpu_t *vcpu_alloc(const char *name, uint64_t entry_ipa, uint64_t vttbr,
                   uint64_t sp_el1_override, const uint8_t *img_src,
                   uint64_t img_dst_pa, uint64_t img_size) {
  vcpu_t *v = &vcpus[nr_vcpus];
  v->id = (uint32_t)nr_vcpus;
  v->vmid = (uint32_t)(nr_vcpus + 1); /* VMID 1, 2 (0 reserved) */
  v->name = name;
  v->vttbr_el2 = vttbr;
  v->entry_ipa = entry_ipa;
  v->sp_el1_init = sp_el1_override;
  v->img_src = img_src;
  v->img_dst_pa = img_dst_pa;
  v->img_size = img_size;
  v->doorbell_target = -1; /* no peer unless wired up after alloc */
  v->weight = 1;           /* equal share by default (VMCTL_WEIGHT changes it) */

  vcpu_init_state(v);

  nr_vcpus++;
  return v;
}

void vcpu_reset(vcpu_t *v, hyp_trap_frame_t *f) {
  hyp_puts("[SCHED] warm-reset VM '");
  hyp_puts(v->name);
  hyp_puts("'\n");

  /* Re-copy the pristine image (the running guest mutated its own .data/.bss
   * and possibly .text) and re-init all execution state. */
  if (v->img_src && v->img_size) {
    hyp_copy_image(v->img_dst_pa, v->img_src, v->img_size);
  }
  vcpu_init_state(v);

  /* Drop any stale TLB entries for this VM's VMID so the fresh guest does not
   * see cached translations from its previous life. */
  s2_tlb_flush_all();

  if (v == cur_vcpu) {
    /* Reset-in-place: reload state into hardware and rewrite the live trap
     * frame so the vector exit erets into the fresh guest. */
    vcpu_load(v);
    for (int i = 0; i < 31; i++) f->regs[i] = v->gp.x[i];
    f->elr = v->gp.elr_el2;
    f->spsr = v->gp.spsr_el2;
  }
}

/* CNTPCT timestamp when the current vCPU was last loaded — used to accumulate
 * per-VM CPU time on each switch-out. */
static uint64_t cur_load_tsc;

static uint64_t read_cntpct(void) {
  uint64_t t;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(t));
  return t;
}

/* Credit the outgoing current vCPU with the wall-time it just ran. */
static void account_cpu_time(void) {
  if (cur_vcpu) {
    uint64_t now = read_cntpct();
    cur_vcpu->cpu_ticks += now - cur_load_tsc;
  }
}

/* Restore the full guest context for `v` into the hardware. */
static void vcpu_load(vcpu_t *v) {
  cur_vcpu = v;
  cur_load_tsc = read_cntpct();
  v->run_count++;
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
#define SCHED_SLICE_TICKS (62500000ULL / 100) /* ~10 ms at 62.5 MHz */

static uint64_t sched_deadline; /* absolute CNTPCT of the next scheduler slice */

/* True if vCPU v has a live (armed, unmasked, not-yet-fired) vtimer deadline. */
static int vtimer_armed(const vcpu_t *v) {
  return (v->vtimer.ctl & 1ULL) /*ENABLE*/ &&
         !(v->vtimer.ctl & 2ULL) /*IMASK*/ && !v->vtimer.pending;
}

/* Arm CNTHP to the soonest of: the scheduler slice deadline, and EVERY vCPU's
 * armed vtimer deadline. Folding in non-current vCPUs is what lets a blocked,
 * idle guest be woken precisely on its own timer while another VM runs. */
void hyp_cnthp_arm(void) {
  uint64_t deadline = sched_deadline;
  for (int i = 0; i < nr_vcpus; i++) {
    if (!vcpus[i].dead && !vcpus[i].paused && vtimer_armed(&vcpus[i]) &&
        vcpus[i].vtimer.cval < deadline) {
      deadline = vcpus[i].vtimer.cval;
    }
  }
  __asm__ __volatile__("msr cnthp_cval_el2, %0" ::"r"(deadline));
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(1ULL));
}

uint64_t hyp_sched_deadline(void) { return sched_deadline; }

/* Arm the next scheduler slice, sized by `v`'s weight (proportional share):
 * a weight-W VM gets W base slices before it is preempted, so over time it
 * receives ~W/(sum of weights) of the CPU. Weight is clamped so one VM can't
 * monopolise or starve. */
#define SCHED_WEIGHT_MAX 16
static void sched_arm_slice_for(const vcpu_t *v) {
  uint32_t w = v ? v->weight : 1;
  if (w < 1) w = 1;
  if (w > SCHED_WEIGHT_MAX) w = SCHED_WEIGHT_MAX;
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
  sched_deadline = now + SCHED_SLICE_TICKS * w;
  hyp_cnthp_arm();
}

void vcpu_sched_init(void) {
  sched_arm_slice_for(cur_vcpu);
  hyp_puts("[SCHED] EL2 weighted scheduler armed (CNTHP PPI 26, 10ms base slice)\n");
}

/* Pick the next runnable (non-dead) vCPU round-robin after `cur`. Returns `cur`
 * if no other vCPU is runnable. */
static vcpu_t *pick_next(vcpu_t *cur) {
  for (int i = 1; i <= nr_vcpus; i++) {
    vcpu_t *cand = &vcpus[(cur->id + i) % nr_vcpus];
    if (cand->runnable && !cand->dead && !cand->paused) return cand;
  }
  return cur;
}

/* Switch the running guest to `next`: save prev's context out of the trap
 * frame + hardware, restore next's into hardware + the frame (so the vector
 * exit erets into next). No-op if next == prev. */
static void switch_to(vcpu_t *next, hyp_trap_frame_t *f) {
  vcpu_t *prev = cur_vcpu;
  if (next == prev) {
    return;
  }
  account_cpu_time(); /* credit prev with the time it just ran */
  static uint64_t nswitch;
  if ((nswitch++ % 200) == 0) {
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

  vcpu_load(next);

  for (int i = 0; i < 31; i++) f->regs[i] = next->gp.x[i];
  f->elr = next->gp.elr_el2;
  f->spsr = next->gp.spsr_el2;
}

/* Inject the timer IRQ into any vCPU whose vtimer deadline has elapsed and mark
 * it runnable — including non-current, blocked ones (so a blocked idle guest
 * wakes on its own timer while another VM runs). Returns 1 if the CURRENT vCPU
 * is blocked and a different vCPU is now runnable (caller should switch). */
int vcpu_wake_expired(void) {
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *v = &vcpus[i];
    if (v->dead || v->paused) continue; /* paused VMs don't wake on their timer */
    if (vtimer_armed(v) && now >= v->vtimer.cval) {
      v->vtimer.pending = 1; /* latch ISTATUS; cleared on guest re-arm */
      if (v == cur_vcpu) {
        vgic_inject_ppi(VTIMER_GUEST_PPI);     /* live LR */
      } else {
        vgic_inject_to(&v->vgic, VTIMER_GUEST_PPI); /* saved LR */
      }
      v->runnable = 1; /* a pending IRQ makes a blocked guest runnable again */
    }
  }

  if (!cur_vcpu->runnable) {
    for (int i = 0; i < nr_vcpus; i++) {
      if (vcpus[i].runnable) return 1; /* someone else can run */
    }
  }
  return 0;
}

/* Periodic scheduler tick (CNTHP slice elapsed): round-robin to the next
 * runnable guest and size the new slice by THAT guest's weight. */
void vcpu_sched_tick(hyp_trap_frame_t *f) {
  vcpu_t *next = pick_next(cur_vcpu);
  sched_arm_slice_for(next); /* weighted slice for whoever runs next */
  switch_to(next, f);
}

/* Current guest did WFI/WFE — block it and switch to another runnable guest.
 * If none is runnable, leave it running (it re-checks WFI); its own timer will
 * fire and wake it. The blocked guest is re-armed via hyp_cnthp_arm folding in
 * all vCPUs' deadlines, and woken by vcpu_wake_expired on the CNTHP fire. */
void vcpu_block_current(hyp_trap_frame_t *f) {
  cur_vcpu->runnable = 0;
  vcpu_t *next = pick_next(cur_vcpu);
  if (next == cur_vcpu) {
    /* Nobody else runnable. Keep this vCPU live (un-block) so it can take its
     * own timer IRQ on the next CNTHP fire. */
    cur_vcpu->runnable = 1;
    return;
  }
  /* Give the VM we switch to its own weighted slice (also re-arms CNTHP, which
   * folds in the blocking guest's vtimer deadline so it still wakes on time). */
  sched_arm_slice_for(next);
  switch_to(next, f);
}
