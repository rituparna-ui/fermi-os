#include "vcpu.h"
#include "hyp.h"
#include "hyp_sysregs.h"
#include "stage2.h"
#include "snapshot.h"
#include "vgic/vgic.h"
#include "timer/vtimer.h"
#include "pcpu.h"
#include "lock.h"
#include "hyp_gic.h"
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

#define MAX_VCPUS 16

static vcpu_t vcpus[MAX_VCPUS];
static int    nr_vcpus;
/* cur_vcpu is now a per-pCPU macro (this_pcpu()->current) from vcpu.h.
 * cur_load_tsc / sched_deadline likewise live in the per-pCPU block, so each
 * physical core tracks its own CPU-time stamp and slice deadline. */
#define cur_load_tsc   (this_pcpu()->cur_load_tsc)
#define sched_deadline (this_pcpu()->sched_deadline)

/* SMP scheduler lock: protects the run-queue scan (vcpus[] scheduler-visible
 * fields runnable/online/dead/paused/weight), CPU-time accounting, and the
 * pcpu_idle_mask. It does NOT own the authoritative on_pcpu write (that is each
 * vCPU's vgic_lock). Taken irqsave (the CNTHP IRQ handler also takes it). */
static hyp_spinlock_t sched_lock = HYP_SPINLOCK_INIT;
/* Bit n set = pCPU n is idle (no runnable vCPU); used to wake an idle core when
 * a vCPU becomes runnable. Written under sched_lock. */
static volatile uint32_t pcpu_idle_mask;
/* Physical SGI INTID the hyp sends core-to-core to force a reschedule / pending
 * drain. SGI 0 in the EL2 (physical) GIC. */
#define HYP_RESCHED_SGI 0

/* Implemented in vcpu_switch.S — load gp->x[]/elr/spsr and eret (no return). */
extern void vcpu_first_entry(vcpu_gp_t *gp) __attribute__((noreturn));

/* Forward decls (definitions appear later in this file). */
static vcpu_t *pick_next_locked(hyp_pcpu_t *pc);
static uint64_t read_cntpct(void);
static void schedule(hyp_trap_frame_t *f);
static void vcpu_load_hw(vcpu_t *v);
static void sched_arm_slice_for(const vcpu_t *v);
static void vgic_drain_pending_locked(vcpu_t *v);
static void hyp_send_resched_sgi(int cpu);

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

  /* vGIC reset: virtual interface enabled, group1 + PMR seeded. SMP vCPUs also
   * enable SGI trapping (TC) so ICC_SGI1R_EL1 routes in software; this must be
   * re-applied here because vgic_vcpu_reset clears hcr to the non-TC default. */
  vgic_vcpu_reset(&v->vgic);
  if (v->is_smp) {
    vgic_enable_sgi_trap(&v->vgic);
  }

  /* vtimer reset: disarmed. */
  v->vtimer.cval = 0;
  v->vtimer.ctl = 0;
  v->vtimer.pending = 0;

  v->runnable = 1;
  v->dead = 0;
}

int vcpu_is_dead(const vcpu_t *v) { return v->dead; }

void vcpu_poweroff_current(hyp_trap_frame_t *f) {
  vcpu_t *me = cur_vcpu;
  uint64_t sl = hyp_lock_irqsave(&sched_lock);
  me->dead = 1;
  me->runnable = 0;
  hyp_unlock_irqrestore(&sched_lock, sl);
  /* schedule() picks another vCPU for this pCPU and rewrites f; if none is
   * claimable it leaves this (now-dead) vCPU as current with f unchanged — the
   * pCPU then idles. With 14 other VMs that does not happen in practice. */
  schedule(f);
  sched_arm_slice_for(cur_vcpu);
}

void vcpu_fault_isolate(hyp_trap_frame_t *f) {
  vcpu_t *v = cur_vcpu;
  v->fault_count++;

  /* Fault budget exceeded -> stop rebooting this VM and power it off, so a
   * guest that faults immediately on every restart can't spin the hypervisor.
   * Also power off if there is no pristine image to reboot from. */
  if (v->fault_count > VCPU_FAULT_MAX || !v->img_src || !v->img_size) {
    hyp_puts("[FAULT] VM '");
    hyp_puts(v->name);
    hyp_puts("' exceeded fault budget — powering it off\n");
    vcpu_poweroff_current(f); /* marks dead + switches away (does not return) */
    return;
  }

  /* Reboot just this VM (warm-reset in place: reloads its pristine image,
   * re-inits state, and rewrites the live trap frame `f` so the vector exit
   * erets into the fresh guest). The other VMs are untouched. */
  hyp_puts("[FAULT] rebooting faulted VM '");
  hyp_puts(v->name);
  hyp_puts("' (fault ");
  hyp_puthex(v->fault_count);
  hyp_puts(" of ");
  hyp_puthex(VCPU_FAULT_MAX);
  hyp_puts(")\n");
  vcpu_reset(v, f);
}

void vcpu_wdog_arm(uint64_t period) {
  cur_vcpu->wdog_period = period;
  if (period == 0) {
    cur_vcpu->wdog_deadline = 0; /* disarmed */
  } else {
    cur_vcpu->wdog_deadline = read_cntpct() + period;
  }
}

void vcpu_check_watchdogs(hyp_trap_frame_t *f) {
  uint64_t now = read_cntpct();
  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *v = &vcpus[i];
    /* Only armed, live, non-paused, ONLINE VMs are watched. A paused VM
     * legitimately isn't petting; a dead one is gone; an OFF (CPU_OFF'd)
     * secondary isn't running so it can't pet — and rebooting it would flush
     * the live primary sibling's shared-VMID TLB and re-copy the image. */
    if (v->wdog_period == 0 || v->dead || v->paused || !v->online) {
      continue;
    }
    if (now < v->wdog_deadline) {
      continue; /* pet in time */
    }
    /* Watchdog expired: the guest hung without petting. Reboot it (and disarm
     * the watchdog — the fresh guest re-arms if it wants). */
    v->wdog_expiries++;
    v->wdog_period = 0;
    v->wdog_deadline = 0;
    hyp_puts("[WDOG] VM '");
    hyp_puts(v->name);
    hyp_puts("' watchdog expired (hung) — rebooting\n");
    vcpu_reset(v, (v == cur_vcpu) ? f : (hyp_trap_frame_t *)0);
  }
}

vcpu_t *vcpu_by_id(int id) {
  return (id >= 0 && id < nr_vcpus) ? &vcpus[id] : (vcpu_t *)0;
}

/* --- SMP: secondary vCPUs, PSCI CPU_ON, SGI routing ----------------------- */

vcpu_t *vcpu_alloc_secondary(vcpu_t *primary, const char *name, uint64_t mpidr) {
  /* Reuse the normal allocator (kstack/struct/defaults), then override the
   * fields that make it a SIBLING of `primary` rather than its own VM:
   * share the stage-2 (vttbr_el2 + vmid) and image; distinct affinity; OFF. */
  vcpu_t *s = vcpu_alloc(name, primary->entry_ipa, primary->vttbr_el2, 0,
                         primary->img_src, primary->img_dst_pa, primary->img_size);
  s->vmid = primary->vmid;          /* same VMID (vcpu_alloc auto-assigned one) */
  s->ram_size = primary->ram_size;
  s->group_id = primary->group_id;  /* sibling of the primary's VM group */
  s->mpidr = mpidr;                 /* distinct affinity */
  s->online = 0;                    /* powered OFF until PSCI CPU_ON */
  s->runnable = 0;
  /* Mark BOTH siblings SMP so each traps ICC_SGI1R_EL1 (TC) for SGI routing.
   * The secondary's TC was just enabled by vcpu_alloc's vcpu_init_state-via-
   * is_smp=0, so re-seed its vGIC now; the primary is re-seeded below. */
  primary->is_smp = 1;
  s->is_smp = 1;
  vgic_enable_sgi_trap(&primary->vgic);
  vgic_enable_sgi_trap(&s->vgic);
  return s;
}

/* Affinity bits of an MPIDR/target value: Aff0[7:0],Aff1[15:8],Aff2[23:16],
 * Aff3[39:32]. Mask off the flag bits [31:24] (U/MT/RES1) for comparison. */
#define MPIDR_AFF_MASK 0xFF00FFFFFFULL

/* Find the in-group sibling of `caller` whose affinity matches `target`. */
static vcpu_t *find_sibling_by_aff(vcpu_t *caller, uint64_t target) {
  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *v = &vcpus[i];
    if (v->group_id != caller->group_id) continue;
    if ((v->mpidr & MPIDR_AFF_MASK) == (target & MPIDR_AFF_MASK)) return v;
  }
  return (vcpu_t *)0;
}

int64_t vcpu_psci_cpu_on(uint64_t target_mpidr, uint64_t entry_ipa,
                         uint64_t context_id) {
  vcpu_t *t = find_sibling_by_aff(cur_vcpu, target_mpidr);
  if (!t) {
    return PSCI_INVALID_PARAMETERS;
  }
  if (t->online) {
    return PSCI_ALREADY_ON;
  }
  /* Bring it up at the requested entry with context_id in x0. Re-init its
   * resettable execution state from the clean reset baseline (MMU off, fresh
   * vGIC/vtimer), then override entry + x0. Do NOT touch its shared stage-2. */
  t->entry_ipa = entry_ipa;
  t->x0_init = context_id;
  vcpu_init_state(t);   /* sets gp.elr=entry, gp.x[0]=context_id, EL1h, online=1 */
  t->online = 1;
  t->runnable = 1;
  hyp_puts("[SMP] CPU_ON: '");
  hyp_puts(t->name);
  hyp_puts("' online at entry ");
  hyp_puthex(entry_ipa);
  hyp_putc('\n');
  return PSCI_SUCCESS;
}

int64_t vcpu_psci_affinity_info(uint64_t target_mpidr) {
  vcpu_t *t = find_sibling_by_aff(cur_vcpu, target_mpidr);
  if (!t) {
    return PSCI_INVALID_PARAMETERS;
  }
  return (t->online && !t->dead) ? 0 /*ON*/ : 1 /*OFF*/;
}

/* PSCI CPU_OFF: the CURRENT vCPU powers ITSELF down. Unlike SYSTEM_OFF it is
 * NOT marked dead — a sibling can CPU_ON it again (real CPU hotplug). Restricted
 * to SMP secondaries (Aff0 != 0); a primary or single-vCPU VM must use SYSTEM_OFF
 * instead, so we never strand a VM with no online sibling to re-CPU_ON it.
 *
 * CONTRACT: on PSCI_SUCCESS this has ALREADY switched away — switch_to() has
 * overwritten the entire trap frame `f` with the NEXT vCPU's context, so the
 * caller (handle_psci) MUST NOT write f->regs[0] afterwards (it would clobber
 * the next vCPU's x0). Only the failure path (PSCI_DENIED) returns a value to
 * the still-running caller. */
int64_t vcpu_psci_cpu_off(hyp_trap_frame_t *f) {
  vcpu_t *me = cur_vcpu;
  if (!me->is_smp || (me->mpidr & 0xFFULL) == 0) {
    return PSCI_DENIED; /* primary / UP VM: use SYSTEM_OFF, not CPU_OFF */
  }

  hyp_con_begin();
  hyp_puts("[SMP] CPU_OFF: '");
  hyp_puts(me->name);
  hyp_puts("' offlining (Aff0=");
  hyp_puthex(me->mpidr & 0xFF);
  hyp_puts(")\n");
  hyp_con_end();

  uint64_t sl = hyp_lock_irqsave(&sched_lock);
  me->online = 0;
  me->runnable = 0;
  me->vtimer.ctl = 0;      /* drop armed vtimer so no iterator folds a stale cval */
  me->vtimer.pending = 0;
  hyp_unlock_irqrestore(&sched_lock, sl);

  /* schedule() picks another vCPU for this pCPU and rewrites f (it will not pick
   * `me` — online=0). On PSCI_SUCCESS f now belongs to the next vCPU, so the
   * caller MUST NOT write f->regs[0]. */
  schedule(f);
  sched_arm_slice_for(cur_vcpu);
  return PSCI_SUCCESS;
}

void vcpu_sgi_route(uint64_t sgi1r) {
  /* ICC_SGI1R_EL1 fields: TargetList[15:0], Aff1[23:16], INTID[27:24],
   * Aff2[39:32], IRM[40], Aff3[55:48]. INTID is a 4-bit SGI (0..15). */
  uint32_t intid = (uint32_t)((sgi1r >> 24) & 0xF);
  int irm = (int)((sgi1r >> 40) & 0x1);
  uint16_t target_list = (uint16_t)(sgi1r & 0xFFFF);
  uint64_t aff1 = (sgi1r >> 16) & 0xFF;
  uint64_t aff2 = (sgi1r >> 32) & 0xFF;
  uint64_t aff3 = (sgi1r >> 48) & 0xFF;

  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *t = &vcpus[i];
    if (t->group_id != cur_vcpu->group_id || !t->online || t->dead) continue;

    if (irm) {
      /* Broadcast to all siblings EXCEPT self. */
      if (t == cur_vcpu) continue;
    } else {
      /* Targeted: higher affinities must match; Aff0 selected by TargetList. */
      uint64_t t_aff1 = (t->mpidr >> 8) & 0xFF;
      uint64_t t_aff2 = (t->mpidr >> 16) & 0xFF;
      uint64_t t_aff3 = (t->mpidr >> 32) & 0xFF;
      uint64_t t_aff0 = t->mpidr & 0xFF;
      if (t_aff1 != aff1 || t_aff2 != aff2 || t_aff3 != aff3) continue;
      if (t_aff0 > 15 || !((target_list >> t_aff0) & 1)) continue;
    }

    /* Route the SGI to the target sibling via the unified injector: live LR if
     * it runs on this pCPU, else latch + reschedule-IPI its core (the cross-core
     * inter-processor SGI path — the SMP guest's ping-pong rides this). */
    vgic_inject(t, intid);
  }
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
  vgic_inject(peer, DOORBELL_INTID); /* routes by residency; marks runnable + kicks */
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
  /* SMP defaults: each VM is its own group, single primary vCPU, affinity 0
   * (bit31 RES1, U=0), online. A secondary is created via vcpu_alloc_secondary
   * which overrides group_id/vttbr/vmid/mpidr and sets online=0. */
  v->group_id = v->id;
  v->mpidr = 0x80000000ULL;
  v->is_smp = 0; /* upgraded to 1 by vcpu_alloc_secondary for SMP VMs */
  v->online = 1;
  /* SMP residency: not running on any pCPU yet; unpinned. The vgic_lock starts
   * free. pending bitmaps start clear (struct is zero-initialised in .bss). */
  v->on_pcpu = -1;
  v->pin_pcpu = -1;
  v->vgic_lock = (hyp_spinlock_t)HYP_SPINLOCK_INIT;

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
    /* Reset-in-place: reload state into hardware and rewrite the live trap frame
     * so the vector exit erets into the fresh guest. v is resident on THIS pCPU
     * (on_pcpu == us, unchanged), so just reload HW + its (freshly reset) vgic. */
    uint64_t vl = hyp_lock_irqsave(&v->vgic_lock);
    vcpu_load_hw(v);
    vgic_restore(&v->vgic);
    vgic_drain_pending_locked(v);
    hyp_unlock_irqrestore(&v->vgic_lock, vl);
    for (int i = 0; i < 31; i++) f->regs[i] = v->gp.x[i];
    f->elr = v->gp.elr_el2;
    f->spsr = v->gp.spsr_el2;
  }
}

/* cur_load_tsc (the CNTPCT timestamp when this pCPU's current vCPU was loaded)
 * is a per-pCPU macro defined at the top of this file. */

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

/* Restore `v`'s hardware context onto THIS pCPU and make it current. Does NOT
 * touch v->vgic save state / on_pcpu — vgic_restore + pending drain + the
 * on_pcpu publish are sequenced by the caller (schedule / vcpu_run_first) under
 * v->vgic_lock, per the residency-ordering rule. */
static void vcpu_load_hw(vcpu_t *v) {
  cur_vcpu = v;            /* per-pCPU: this_pcpu()->current */
  cur_load_tsc = read_cntpct();
  v->run_count++;
  vcpu_restore_sysregs(&v->sys);
  vcpu_restore_fp(&v->fp);
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(v->vttbr_el2));
  /* Per-vCPU MPIDR: the guest reads its own affinity via MPIDR_EL1 (= VMPIDR_EL2
   * at EL1). Reloaded per world switch so SMP siblings read distinct affinities. */
  __asm__ __volatile__("msr vmpidr_el2, %0" ::"r"(v->mpidr));
  vtimer_reprogram_current(); /* re-arm CNTHP to this guest's shadow deadline */
}

/* First-ever entry on a pCPU. On pCPU0 this loads vcpus[0] (dom0/FermiOS order);
 * on a secondary it claims a distinct runnable vCPU. Does not return. */
__attribute__((noreturn)) void vcpu_run_first(void) {
  hyp_pcpu_t *pc = this_pcpu();
  uint64_t sl = hyp_lock_irqsave(&sched_lock);
  vcpu_t *v = pick_next_locked(pc);
  if (!v) {
    /* No claimable vCPU for this core — should not happen for pCPU0. */
    hyp_unlock_irqrestore(&sched_lock, sl);
    hyp_panic("vcpu_run_first: no runnable vCPU");
  }
  pcpu_idle_mask &= ~(1u << pc->cpu_id);
  sched_arm_slice_for(v);
  hyp_unlock_irqrestore(&sched_lock, sl);

  uint64_t vl = hyp_lock_irqsave(&v->vgic_lock);
  vcpu_load_hw(v);
  vgic_restore(&v->vgic);
  vgic_drain_pending_locked(v);
  v->on_pcpu = (int)pc->cpu_id;
  hyp_unlock_irqrestore(&v->vgic_lock, vl);

  vcpu_first_entry(&v->gp);
}

/* --- EL2 scheduler tick via the EL2 PHYSICAL timer (CNTHP_EL2, PPI 26) -----
 * We reuse CNTHP — the same EL2 physical timer the vtimer uses — because its
 * IRQ delivery to EL2 is proven (the EL2 virtual timer CNTHV/PPI 28 does not
 * deliver reliably on this QEMU). The vtimer and the scheduler share CNTHP by
 * always arming it to the SOONER of {the running guest's vtimer deadline, the
 * next scheduler slice}. On each CNTHP fire the IRQ handler services whichever
 * deadline(s) elapsed. A coarse slice (~50 ms) keeps the round-robin visible. */
#define SCHED_SLICE_TICKS (62500000ULL / 100) /* ~10 ms at 62.5 MHz */

/* sched_deadline (absolute CNTPCT of THIS pCPU's next scheduler slice) is a
 * per-pCPU macro defined at the top of this file. */

/* True if vCPU v has a live (armed, unmasked, not-yet-fired) vtimer deadline. */
static int vtimer_armed(const vcpu_t *v) {
  return (v->vtimer.ctl & 1ULL) /*ENABLE*/ &&
         !(v->vtimer.ctl & 2ULL) /*IMASK*/ && !v->vtimer.pending;
}

/* Arm THIS pCPU's CNTHP to the soonest of: this core's scheduler slice deadline,
 * and the armed vtimer deadline of vCPUs RESIDENT ON or CLAIMABLE BY this core.
 *
 * SMP change: each pCPU folds in ONLY the vtimers relevant to it — its resident
 * vCPU, plus parked (on_pcpu<0) claimable vCPUs (whose timer wake this core may
 * service). It must NOT fold in vCPUs resident on OTHER pCPUs (their own core
 * handles their timer) — doing so caused an N-way cross-core CNTHP re-fire
 * storm. The online gate is still mandatory (an OFF secondary's stale timer). */
void hyp_cnthp_arm(void) {
  hyp_pcpu_t *pc = this_pcpu();
  uint64_t deadline = sched_deadline;
  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *v = &vcpus[i];
    if (!v->online || v->dead || v->paused || !vtimer_armed(v)) {
      continue;
    }
    /* Only this core's resident vCPU, or a parked (unowned) one this core could
     * run. Pinned-to-another-core vCPUs are also skipped. */
    int mine = (v->on_pcpu == (int)pc->cpu_id) ||
               (v->on_pcpu < 0 &&
                (v->pin_pcpu < 0 || v->pin_pcpu == (int)pc->cpu_id));
    if (mine && v->vtimer.cval < deadline) {
      deadline = v->vtimer.cval;
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

/* Pick a runnable vCPU for pCPU `pc`, round-robin after its current. Caller
 * holds sched_lock. Returns NULL if nothing is claimable (the core should idle).
 * A vCPU resident on another pCPU (on_pcpu >= 0 and != us) is skipped — a vCPU
 * runs on at most one pCPU at a time (no double-run). pin_pcpu restricts a vCPU
 * to one core (used to pin the SMP guest in Phase C). */
static vcpu_t *pick_next_locked(hyp_pcpu_t *pc) {
  vcpu_t *cur = pc->current;
  int start = cur ? (int)cur->id : 0;
  for (int i = 1; i <= nr_vcpus; i++) {
    vcpu_t *c = &vcpus[(start + i) % nr_vcpus];
    if (c->runnable && c->online && !c->dead && !c->paused &&
        (c->pin_pcpu < 0 || c->pin_pcpu == (int)pc->cpu_id) &&
        (c->on_pcpu < 0 || c == cur)) {
      return c;
    }
  }
  /* Keep running cur if it is still claimable by us. */
  if (cur && cur->runnable && cur->online && !cur->dead && !cur->paused) {
    return cur;
  }
  return (vcpu_t *)0;
}

/* The one true reschedule path. Picks the next vCPU for THIS pCPU under
 * sched_lock, then hands off: saves prev WHILE STILL OWNING IT (clearing
 * on_pcpu only AFTER the save, under prev's vgic_lock), claims next (setting
 * on_pcpu only AFTER restore+drain, under next's vgic_lock). This ordering is
 * what makes cross-core injection race-free. Rewrites the trap frame `f` so the
 * vector exit erets into next. If no vCPU is claimable, the pCPU goes idle. */
static void schedule(hyp_trap_frame_t *f) {
  hyp_pcpu_t *pc = this_pcpu();

  uint64_t sl = hyp_lock_irqsave(&sched_lock);
  vcpu_t *prev = pc->current;
  vcpu_t *next = pick_next_locked(pc);
  if (next == prev) {
    hyp_unlock_irqrestore(&sched_lock, sl);
    return; /* nothing to do (also the common single-runnable case) */
  }
  account_cpu_time(); /* credit prev under sched_lock (reads cur_load_tsc) */
  if (next) {
    pcpu_idle_mask &= ~(1u << pc->cpu_id);
  } else {
    pcpu_idle_mask |= (1u << pc->cpu_id);
  }
  static uint64_t nswitch;
  uint64_t n = nswitch++;
  hyp_unlock_irqrestore(&sched_lock, sl);

  if ((n % 200) == 0) {
    hyp_con_begin();
    hyp_puts("[SCHED][cpu");
    hyp_puthex(pc->cpu_id);
    hyp_puts("] world switches: ");
    hyp_puthex(n);
    hyp_putc('\n');
    hyp_con_end();
  }

  /* Save prev while we still own it (on_pcpu unchanged until after the save). */
  if (prev) {
    for (int i = 0; i < 31; i++) prev->gp.x[i] = f->regs[i];
    prev->gp.elr_el2 = f->elr;
    prev->gp.spsr_el2 = f->spsr;
    vcpu_save_sysregs(&prev->sys);
    vcpu_save_fp(&prev->fp);
    uint64_t vl = hyp_lock_irqsave(&prev->vgic_lock);
    vgic_save(&prev->vgic);
    prev->on_pcpu = -1; /* publish: not resident anywhere (AFTER save) */
    hyp_unlock_irqrestore(&prev->vgic_lock, vl);
  }

  if (!next) {
    /* No claimable vCPU: go idle. cur_vcpu (== pc->current) becomes NULL; the
     * core wfi's and is woken by CNTHP or a reschedule SGI, then re-runs
     * schedule() from the IRQ return path. */
    pc->current = (void *)0;
    return;
  }

  uint64_t vl2 = hyp_lock_irqsave(&next->vgic_lock);
  vcpu_load_hw(next);          /* sets pc->current = next (owner-only write) */
  vgic_restore(&next->vgic);
  vgic_drain_pending_locked(next);
  next->on_pcpu = (int)pc->cpu_id; /* publish residency AFTER restore+drain */
  hyp_unlock_irqrestore(&next->vgic_lock, vl2);

  for (int i = 0; i < 31; i++) f->regs[i] = next->gp.x[i];
  f->elr = next->gp.elr_el2;
  f->spsr = next->gp.spsr_el2;
}

/* --- SMP: cross-core vGIC injection + reschedule IPI ---------------------- */

/* Send a physical reschedule SGI (HYP_RESCHED_SGI) to physical core `cpu` so it
 * re-evaluates: drains its current vCPU's pending vIRQ bitmap into live LRs, or
 * (if idle) breaks its wfi and re-runs schedule(). */
static void hyp_send_resched_sgi(int cpu) {
  uint64_t aff0 = hyp_pcpus[cpu].mpidr & 0xFF;
  /* ICC_SGI1R_EL1: INTID[27:24], TargetList[15:0]; Aff1/2/3 = 0 on QEMU virt. */
  uint64_t sgi1r = ((uint64_t)HYP_RESCHED_SGI << 24) | (1ULL << aff0);
  __asm__ __volatile__("msr icc_sgi1r_el1, %0\n\tisb" ::"r"(sgi1r) : "memory");
}

/* Drain a vCPU's pending bitmap into the LIVE List Registers. Caller holds
 * v->vgic_lock and v is resident on THIS pCPU (so the live LRs are its). If an
 * LR is full the bit is kept pending (re-set) so the interrupt is not lost. */
static void vgic_drain_pending_locked(vcpu_t *v) {
  uint32_t lo = v->vgic.pending_lo, hi = v->vgic.pending_hi;
  v->vgic.pending_lo = 0;
  v->vgic.pending_hi = 0;
  for (uint32_t i = 0; i < 32; i++) {
    if ((lo & (1u << i)) && !vgic_try_inject_live(i)) {
      v->vgic.pending_lo |= (1u << i); /* LR full -> keep pending (not lost) */
    }
  }
  for (uint32_t i = 0; i < 32; i++) {
    if ((hi & (1u << i)) && !vgic_try_inject_live(i + 32)) {
      v->vgic.pending_hi |= (1u << i);
    }
  }
}

/* Unified vIRQ injection into target vCPU `t` (any INTID 0..63). Routes by
 * residency, race-free via on_pcpu published under t->vgic_lock:
 *  - resident on THIS pCPU -> write its live LR now;
 *  - resident on ANOTHER pCPU -> latch in its pending bitmap + reschedule-SGI
 *    that core so it drains into its live LRs;
 *  - not running -> latch in the bitmap, mark runnable (under sched_lock), and
 *    kick an idle pCPU to pick it up. Never drops (overflow stays pending). */
void vgic_inject(vcpu_t *t, uint32_t intid) {
  hyp_pcpu_t *pc = this_pcpu();
  uint64_t fl = hyp_lock_irqsave(&t->vgic_lock);
  if (t->on_pcpu == (int)pc->cpu_id) {
    /* Resident here: straight to the live LRs (fall back to bitmap if full). */
    if (!vgic_try_inject_live(intid)) {
      vgic_set_pending(&t->vgic, intid);
    }
    hyp_unlock_irqrestore(&t->vgic_lock, fl);
    return;
  }
  vgic_set_pending(&t->vgic, intid);
  int resident = t->on_pcpu;
  hyp_unlock_irqrestore(&t->vgic_lock, fl);

  /* Mark runnable (sched_lock owns it) + decide who to kick. */
  uint64_t sl = hyp_lock_irqsave(&sched_lock);
  t->runnable = 1;
  int idle = -1;
  if (resident < 0) {
    for (int c = 0; c < HYP_MAX_PCPUS; c++) {
      if (pcpu_idle_mask & (1u << c)) { idle = c; break; }
    }
  }
  hyp_unlock_irqrestore(&sched_lock, sl);

  if (resident >= 0)   hyp_send_resched_sgi(resident); /* poke its core to drain */
  else if (idle >= 0)  hyp_send_resched_sgi(idle);     /* wake an idle core */
}

/* Inject the timer IRQ into any vCPU whose vtimer deadline elapsed. SMP: this
 * pCPU services ONLY vCPUs it could run — its resident one (live LR) and parked
 * (on_pcpu<0) ones (via vgic_inject, which latches + kicks). vCPUs resident on
 * OTHER pCPUs are skipped (their own core's CNTHP handles their timer). Returns
 * 1 if THIS core's current vCPU is blocked and should reschedule. */
int vcpu_wake_expired(void) {
  hyp_pcpu_t *pc = this_pcpu();
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  for (int i = 0; i < nr_vcpus; i++) {
    vcpu_t *v = &vcpus[i];
    if (v->dead || v->paused || !v->online) continue;
    /* Only my resident vCPU or a parked one I could run. */
    int mine = (v->on_pcpu == (int)pc->cpu_id) ||
               (v->on_pcpu < 0 &&
                (v->pin_pcpu < 0 || v->pin_pcpu == (int)pc->cpu_id));
    if (!mine) continue;
    if (vtimer_armed(v) && now >= v->vtimer.cval) {
      v->vtimer.pending = 1;       /* latch ISTATUS; cleared on guest re-arm */
      vgic_inject(v, VTIMER_GUEST_PPI);
      v->runnable = 1;
    }
  }

  vcpu_t *me = cur_vcpu;
  if (me && !me->runnable) {
    uint64_t sl = hyp_lock_irqsave(&sched_lock);
    int other = 0;
    for (int i = 0; i < nr_vcpus; i++) {
      if (vcpus[i].runnable && vcpus[i].online && vcpus[i].on_pcpu < 0) { other = 1; break; }
    }
    hyp_unlock_irqrestore(&sched_lock, sl);
    return other;
  }
  return 0;
}

/* Periodic scheduler tick (this pCPU's CNTHP slice elapsed): reschedule + arm
 * the next slice. Watchdog liveness check first. */
void vcpu_sched_tick(hyp_trap_frame_t *f) {
  vcpu_check_watchdogs(f);
  schedule(f);
  sched_arm_slice_for(cur_vcpu); /* arm slice for whoever now runs here (may be NULL) */
}

/* Current guest did WFI/WFE — block it and reschedule. If nothing else is
 * claimable on this pCPU, schedule() leaves it running (pick keeps cur) — its
 * own timer wakes it. */
void vcpu_block_current(hyp_trap_frame_t *f) {
  vcpu_t *me = cur_vcpu;
  if (me) {
    uint64_t sl = hyp_lock_irqsave(&sched_lock);
    me->runnable = 0;
    hyp_unlock_irqrestore(&sched_lock, sl);
  }
  schedule(f);
  /* If schedule() found nothing else, cur is still `me` and we re-mark it
   * runnable so it can take its own timer IRQ. */
  if (cur_vcpu == me && me) {
    uint64_t sl = hyp_lock_irqsave(&sched_lock);
    me->runnable = 1;
    hyp_unlock_irqrestore(&sched_lock, sl);
  }
  sched_arm_slice_for(cur_vcpu);
}
