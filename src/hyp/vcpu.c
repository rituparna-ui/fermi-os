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

#define MAX_VCPUS 16

static vcpu_t vcpus[MAX_VCPUS];
static int    nr_vcpus;
vcpu_t       *cur_vcpu;

/* Implemented in vcpu_switch.S — load gp->x[]/elr/spsr and eret (no return). */
extern void vcpu_first_entry(vcpu_gp_t *gp) __attribute__((noreturn));

/* Forward decls (definitions appear later in this file). */
static vcpu_t *pick_next(vcpu_t *cur);
static uint64_t read_cntpct(void);
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
  cur_vcpu->dead = 1;
  cur_vcpu->runnable = 0;
  vcpu_t *next = pick_next(cur_vcpu);
  if (next == cur_vcpu) {
    hyp_panic("last VM powered off — nothing left to run");
  }
  switch_to(next, f);
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
  if (!cur_vcpu->is_smp || (cur_vcpu->mpidr & 0xFFULL) == 0) {
    return PSCI_DENIED; /* primary / UP VM: use SYSTEM_OFF, not CPU_OFF */
  }

  hyp_puts("[SMP] CPU_OFF: '");
  hyp_puts(cur_vcpu->name);
  hyp_puts("' offlining (Aff0=");
  hyp_puthex(cur_vcpu->mpidr & 0xFF);
  hyp_puts(")\n");

  cur_vcpu->online = 0;
  cur_vcpu->runnable = 0;
  /* Defensive: drop any armed vtimer deadline so no vCPU-iterating site can fold
   * a stale cval (vcpu_init_state re-clears these on the next CPU_ON anyway). */
  cur_vcpu->vtimer.ctl = 0;
  cur_vcpu->vtimer.pending = 0;

  vcpu_t *next = pick_next(cur_vcpu);
  if (next == cur_vcpu) {
    /* Unreachable in this design (the primary sibling + other VMs stay
     * runnable), but switching to self would re-run the vCPU we just offlined.
     * Re-online and refuse rather than corrupt state. */
    cur_vcpu->online = 1;
    cur_vcpu->runnable = 1;
    return PSCI_DENIED;
  }
  /* account_cpu_time() inside switch_to credits the offlining vCPU (cur_vcpu is
   * still it until vcpu_load) — do not reorder. */
  switch_to(next, f); /* rewrites f; the vector exit erets into `next` */
  return PSCI_SUCCESS; /* MUST NOT be written into f — the frame is now next's */
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

    /* Deliver like the doorbell: live LR if it's the current vCPU, else its
     * saved LR; wake it if blocked. */
    if (t == cur_vcpu) {
      vgic_inject_ppi(intid);
    } else {
      vgic_inject_to(&t->vgic, intid);
      t->runnable = 1;
    }
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
  /* SMP defaults: each VM is its own group, single primary vCPU, affinity 0
   * (bit31 RES1, U=0), online. A secondary is created via vcpu_alloc_secondary
   * which overrides group_id/vttbr/vmid/mpidr and sets online=0. */
  v->group_id = v->id;
  v->mpidr = 0x80000000ULL;
  v->is_smp = 0; /* upgraded to 1 by vcpu_alloc_secondary for SMP VMs */
  v->online = 1;

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
  /* Per-vCPU MPIDR: the guest reads its own affinity via MPIDR_EL1 (= VMPIDR_EL2
   * at EL1). Set ONCE at boot today, so it MUST be reloaded per world switch or
   * SMP siblings would all read the same affinity. */
  __asm__ __volatile__("msr vmpidr_el2, %0" ::"r"(v->mpidr));
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
    /* online gate is MANDATORY: an OFF (CPU_OFF'd) secondary with a stale armed
     * vtimer would otherwise pull CNTHP to a past deadline, which fires ->
     * vcpu_wake_expired skips it (!online) -> re-arm to the same past deadline
     * -> a tight CNTHP re-fire livelock. */
    if (vcpus[i].online && !vcpus[i].dead && !vcpus[i].paused &&
        vtimer_armed(&vcpus[i]) && vcpus[i].vtimer.cval < deadline) {
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
    /* online: a PSCI-OFF secondary (online=0) must never be scheduled until
     * CPU_ON, even though its runnable flag may be set by vcpu_init_state. */
    if (cand->runnable && cand->online && !cand->dead && !cand->paused)
      return cand;
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
    if (v->dead || v->paused || !v->online) continue; /* off/paused don't wake */
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
      /* online gate: an OFF vCPU must not count as "someone else can run" even
       * if a stale runnable flag lingers. */
      if (vcpus[i].runnable && vcpus[i].online) return 1;
    }
  }
  return 0;
}

/* Periodic scheduler tick (CNTHP slice elapsed): round-robin to the next
 * runnable guest and size the new slice by THAT guest's weight. */
void vcpu_sched_tick(hyp_trap_frame_t *f) {
  /* Liveness check first: reboot any VM whose watchdog expired (hung). For a
   * non-current expired VM this just resets its struct (takes effect when it is
   * next loaded); for the current one it reloads HW + rewrites f in place. */
  vcpu_check_watchdogs(f);

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
