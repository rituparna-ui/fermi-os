#ifndef HYP_VCPU_H
#define HYP_VCPU_H

#include <stdint.h>
#include "vm.h" /* hyp_trap_frame_t */

/* ---------------------------------------------------------------------------
 * Per-virtual-CPU state for a multi-VM type-1 hypervisor.
 *
 * Each guest VM has exactly one vCPU (single-core guests). When the EL2
 * scheduler world-switches from one vCPU to another it must save the outgoing
 * guest's complete EL1/EL0 + FP + virtual-device state and restore the
 * incoming guest's — otherwise a register left behind silently corrupts the
 * other guest. The set below is the canonical KVM-style context.
 *
 * Banking note: ELR_EL1/SPSR_EL1/SP_EL1/SP_EL0 are banked separately from the
 * EL2 versions, so the *hypervisor's own* EL2 execution never clobbers them —
 * but a world switch to another guest DOES (we load the other guest's values),
 * so they must be saved/restored.
 * ------------------------------------------------------------------------- */

/* GP + EL2-return state mirrored from the trap frame. */
typedef struct {
  uint64_t x[31];  /* x0..x30 */
  uint64_t elr_el2;  /* guest PC to resume    */
  uint64_t spsr_el2; /* guest PSTATE to resume */
} vcpu_gp_t;

/* EL1 system + exception state (saved/restored by vcpu_switch.S). Field order
 * MUST match the asm in vcpu_switch.S. */
typedef struct {
  uint64_t sctlr_el1;
  uint64_t cpacr_el1;
  uint64_t ttbr0_el1;
  uint64_t ttbr1_el1;
  uint64_t tcr_el1;
  uint64_t mair_el1;
  uint64_t amair_el1;
  uint64_t vbar_el1;
  uint64_t contextidr_el1;
  uint64_t tpidr_el0;
  uint64_t tpidrro_el0;
  uint64_t tpidr_el1;
  uint64_t sp_el0;
  uint64_t sp_el1;
  uint64_t elr_el1;
  uint64_t spsr_el1;
  /* NOTE: ESR_EL1 / FAR_EL1 / PAR_EL1 are deliberately NOT part of the saved
   * context. They are read-only exception-state registers (writes are
   * CONSTRAINED UNPREDICTABLE / ignored) and are ephemeral — hardware rewrites
   * them on the guest's next exception. A guest reads them immediately after
   * an exception, before any world switch, so they never need preserving. */
} vcpu_sysregs_t;

/* FP/SIMD state (q0..q31 + FPSR/FPCR). Saved/restored by vcpu_switch.S.
 * Kept 16-byte aligned for the q-register stores. */
typedef struct {
  uint8_t  q[32 * 16]; /* 32 x 128-bit vectors */
  uint64_t fpsr;
  uint64_t fpcr;
} __attribute__((aligned(16))) vcpu_fp_t;

/* Virtual GICv3 per-vCPU state (the hardware ICH_* regs are shared, so they
 * are saved out on exit and restored on entry). */
typedef struct vcpu_vgic {
  uint64_t hcr;      /* ICH_HCR_EL2  */
  uint64_t vmcr;     /* ICH_VMCR_EL2 */
  uint64_t ap0r0;    /* ICH_AP0R0_EL2 */
  uint64_t ap1r0;    /* ICH_AP1R0_EL2 */
  uint64_t lr[16];   /* ICH_LR0..15_EL2 (only [0,nr_lr) used) */
  /* GICD/GICR MMIO software model. */
  uint32_t gicd_ctlr;
  uint32_t gicd_isenabler0; /* SPIs/PPIs 0..31  */
  uint32_t gicd_isenabler1; /* SPIs 32..63 (doorbell INTID 40 lives here) */
  uint32_t gicr_igroupr0;
  uint32_t gicr_igrpmodr0;
  uint32_t gicr_isenabler0;
} vcpu_vgic_t;

/* Per-vCPU exit statistics (xentop-style observability). Bumped by the EL2
 * trap dispatcher; surfaced to dom0 via VMCTL_STAT. */
typedef struct {
  uint64_t hvc;        /* hypercalls (yield/doorbell/vmctl/PSCI) */
  uint64_t data_abort; /* stage-2 / MMIO data aborts            */
  uint64_t sysreg;     /* trapped CNTP_* sysreg accesses        */
  uint64_t wfx;        /* WFI/WFE traps                         */
  uint64_t irq;        /* physical IRQs taken at EL2 for this VM */
} vcpu_stats_t;

/* Virtual timer per-vCPU state. */
typedef struct {
  uint64_t cval;    /* shadow CNTP_CVAL_EL0 */
  uint64_t ctl;     /* shadow CNTP_CTL_EL0 (ENABLE|IMASK bits)  */
  int      pending; /* timer condition latched (ISTATUS) — set when CNTHP
                     * fires, surfaced in CNTP_CTL reads, cleared when the
                     * guest re-arms CNTP_CVAL/CNTP_TVAL. */
} vcpu_vtimer_t;

typedef struct vcpu {
  vcpu_gp_t      gp;
  vcpu_sysregs_t sys;
  vcpu_fp_t      fp;
  vcpu_vgic_t    vgic;
  vcpu_vtimer_t  vtimer;
  vcpu_stats_t   stats;

  uint64_t vttbr_el2; /* stage-2 base | (VMID << 48) — per-VM address space */
  uint32_t vmid;
  uint32_t id;        /* 0, 1, ... */
  const char *name;

  /* SMP: sibling vCPUs of one VM share group_id + vttbr_el2 + vmid but each has
   * a distinct mpidr (affinity). A secondary starts powered OFF (online=0) and
   * is brought up by PSCI CPU_ON. Single-vCPU VMs have a unique group_id, mpidr
   * 0x80000000, and online=1. */
  uint32_t group_id;  /* links sibling vCPUs of one SMP VM */
  uint64_t mpidr;     /* per-vCPU VMPIDR_EL2 (affinity; bit31 RES1, U=0) */
  int      is_smp;    /* 1 = part of a multi-vCPU VM: enable ICH_HCR_EL2.TC so
                       * ICC_SGI1R_EL1 traps for software inter-CPU SGI routing.
                       * Single-vCPU VMs leave this 0 (TC would also trap PMR). */
  int      online;    /* 0 = powered off (pre-CPU_ON / CPU_OFF), never scheduled */
  int runnable;       /* 0 = blocked on WFI awaiting its next interrupt */
  int dead;           /* 1 = powered off (PSCI SYSTEM_OFF), never runs again */

  /* Pristine image, for PSCI SYSTEM_RESET (warm restart). On reset the
   * hypervisor re-copies [img_src, img_src+img_size) to img_dst_pa (the host
   * PA backing the guest's load IPA), then re-initialises register state and
   * re-enters at entry_ipa. */
  const uint8_t *img_src;
  uint64_t       img_dst_pa;  /* host PA backing the guest's RAM base IPA */
  uint64_t       img_size;
  uint64_t       ram_size;    /* size of the guest's private RAM window  */
  uint64_t       entry_ipa;   /* guest RAM base IPA == entry == 0x40000000 */
  uint64_t       sp_el1_init;
  uint64_t       x0_init;   /* value placed in guest x0 at (re)start — used to
                             * pass a role/arg to the guest (e.g. IPC producer
                             * vs consumer). */
  int            doorbell_target; /* vCPU id to notify on HVC_FERMI_DOORBELL,
                                   * or -1 if this VM has no peer. */
  int            privileged; /* 1 = may issue management hypercalls (VMCTL)
                              * against other VMs — the "dom0" control domain. */
  int            paused;     /* 1 = administratively paused (VMCTL_STOP); the
                              * scheduler skips it and does NOT auto-resume it
                              * on a timer wake (distinct from WFI-blocked). */
  uint32_t       weight;     /* proportional CPU share (Xen-credit / cgroup
                              * cpu.weight style). Slice length scales with it;
                              * default 1. Set via VMCTL_WEIGHT. */
  uint64_t       run_count;  /* times this vCPU has been scheduled in */
  uint64_t       cpu_ticks;  /* total CNTPCT ticks this vCPU has run (for
                              * proportional-share accounting / VMCTL_CPUTIME) */
  uint32_t       fault_count; /* unhandled EL2 traps this VM has caused; after
                               * VCPU_FAULT_MAX the hypervisor stops rebooting
                               * it and powers it off (avoids a reset loop). */
  /* Liveness watchdog: a guest arms it (HVC_FERMI_WDOG, x1=timeout ticks) and
   * must "pet" it before the deadline; if it stops (hangs/livelocks without
   * faulting), the scheduler reboots it. Catches hangs the way fault isolation
   * catches crashes. wdog_period 0 = disarmed. */
  uint64_t       wdog_period;   /* CNTPCT ticks between required pets (0 = off) */
  uint64_t       wdog_deadline; /* absolute CNTPCT by which the next pet is due */
  uint32_t       wdog_expiries; /* count of watchdog-triggered reboots */
} vcpu_t;

/* Reboot-on-fault budget: after this many unhandled traps, kill instead of
 * rebooting (a guest that faults immediately on every restart would otherwise
 * spin the hypervisor forever). */
#define VCPU_FAULT_MAX 3

/* The currently-running vCPU on the CALLING physical core (set by the scheduler
 * before each guest entry). vtimer / vgic / device emulation use this to find
 * the per-VM state of the VM that trapped on THIS pCPU. On the uniprocessor this
 * was a single global; under SMP it is per-pCPU (this_pcpu()->current). The
 * macro is a valid lvalue, so vcpu_load's `cur_vcpu = v` still works. */
#include "pcpu.h"
#define cur_vcpu (this_pcpu()->current)

/* Save the outgoing guest's EL1 sysregs + FP into `v`; restore the incoming
 * guest's into the CPU. Implemented in vcpu_switch.S. */
void vcpu_save_sysregs(vcpu_sysregs_t *s);
void vcpu_restore_sysregs(const vcpu_sysregs_t *s);
void vcpu_save_fp(vcpu_fp_t *f);
void vcpu_restore_fp(const vcpu_fp_t *f);

/* Allocate a vCPU: enters `name` at `entry_ipa` (EL1h) with stage-2 base
 * `vttbr`; sp_el1_override (0 = leave at reset baseline) sets the initial
 * guest SP_EL1. img_src/img_dst_pa/img_size describe the pristine guest image
 * (re-copied on PSCI SYSTEM_RESET); pass img_src=0 to disable warm-reset. */
vcpu_t *vcpu_alloc(const char *name, uint64_t entry_ipa, uint64_t vttbr,
                   uint64_t sp_el1_override, const uint8_t *img_src,
                   uint64_t img_dst_pa, uint64_t img_size);

/* Warm-reset a vCPU: re-copy its pristine image, re-init register/timer/vGIC
 * state, mark runnable. If `v` is the CURRENT vCPU, `f` (its live trap frame)
 * is rewritten so the vector exit erets into the fresh guest; pass f=0 when
 * resetting a non-current vCPU. */
void vcpu_reset(vcpu_t *v, hyp_trap_frame_t *f);

/* Power off the current VM (PSCI SYSTEM_OFF): mark it permanently dead and
 * switch to another runnable VM. If it is the last one, the hypervisor halts. */
void vcpu_poweroff_current(hyp_trap_frame_t *f);

/* Fault-isolate the CURRENT VM after an unhandled EL2 trap: instead of panicking
 * the whole machine, reboot just this VM (warm-reset in place) — or, once it has
 * exceeded VCPU_FAULT_MAX reboots, power it off — and let the others keep
 * running. `f` is the faulting VM's live trap frame. Does not return to the
 * faulting instruction. */
void vcpu_fault_isolate(hyp_trap_frame_t *f);

/* Arm or pet the CURRENT VM's liveness watchdog (HVC_FERMI_WDOG). period == 0
 * disarms it; otherwise (re)sets the deadline to now + period ticks. The guest
 * must call this again before the deadline or the scheduler reboots it. */
void vcpu_wdog_arm(uint64_t period);

/* Called on each scheduler tick: reboot any VM whose armed watchdog deadline has
 * passed (it hung without petting). If the CURRENT vCPU is the one that expired,
 * `f` is rewritten so the vector exit erets into the rebooted guest. */
void vcpu_check_watchdogs(hyp_trap_frame_t *f);

/* Ring the doorbell from `from` to its configured peer: inject DOORBELL_INTID
 * into the peer (live List Register if it is current, saved state otherwise)
 * and mark it runnable. Returns 0 on success, -1 if `from` has no peer. */
int vcpu_ring_doorbell(vcpu_t *from);

/* Look up a vCPU by id (for wiring peers). */
vcpu_t *vcpu_by_id(int id);

/* Allocate a SECONDARY (AP) vCPU for an SMP VM: inherits the primary's stage-2
 * (vttbr_el2/vmid), group_id, and image fields; gets the given distinct mpidr
 * affinity; starts powered OFF (online=0) until PSCI CPU_ON. Returns it. */
vcpu_t *vcpu_alloc_secondary(vcpu_t *primary, const char *name, uint64_t mpidr);

/* PSCI CPU_ON: power on the in-group sibling whose affinity matches
 * target_mpidr, entering it at entry_ipa with x0 = context_id. Returns a PSCI
 * status (SUCCESS / ALREADY_ON / INVALID_PARAMETERS). */
int64_t vcpu_psci_cpu_on(uint64_t target_mpidr, uint64_t entry_ipa,
                         uint64_t context_id);

/* PSCI AFFINITY_INFO: 0 = ON, 1 = OFF for the in-group sibling matching
 * target_mpidr; INVALID_PARAMETERS if none. */
int64_t vcpu_psci_affinity_info(uint64_t target_mpidr);

/* PSCI CPU_OFF: the CURRENT vCPU powers itself down (no target arg). Allowed
 * only for SMP secondaries (Aff0 != 0); a primary / single-vCPU VM must use
 * SYSTEM_OFF. On PSCI_SUCCESS it has switched away and `f` now belongs to the
 * NEXT vCPU — the caller MUST NOT write f->regs[0]. On failure returns
 * PSCI_DENIED to the still-running caller. */
int64_t vcpu_psci_cpu_off(hyp_trap_frame_t *f);

/* Route a guest SGI (decoded from ICC_SGI1R_EL1) to its in-group target
 * sibling(s): inject `intid` (an SGI 0..15) into each, waking blocked ones. */
void vcpu_sgi_route(uint64_t sgi1r);

/* Management hypercall (HVC_FERMI_VMCTL) from a privileged "dom0" control VM.
 * op/target/arg are x1/x2/x3 from the caller's frame; returns the x0 result.
 * `f` is the caller's live frame (needed if a RESET targets the caller itself).
 * Rejects with VMCTL_EPERM unless cur_vcpu->privileged. */
int64_t vcpu_vmctl(uint64_t op, uint64_t target, uint64_t arg,
                   hyp_trap_frame_t *f);

/* Total number of vCPUs. */
int vcpu_count(void);

/* Translate a guest RAM IPA to its host PA for the given vCPU, bounds-checked
 * against [entry_ipa, entry_ipa+ram_size). Returns 0 if out of range. (Only
 * the linear private-RAM window; device/shared regions are not translated.) */
uint64_t vcpu_ipa_to_pa(const vcpu_t *v, uint64_t ipa, uint64_t len);

/* PV console: print [buf_ipa, buf_ipa+len) from the CURRENT guest's RAM to the
 * host console, tagged with the VM name. Returns bytes printed, or -1 on a bad
 * pointer/length. len is clamped to a sane max. */
int64_t vcpu_pv_log(uint64_t buf_ipa, uint64_t len);

/* True once a vCPU has been powered off (never runs again). */
int vcpu_is_dead(const vcpu_t *v);

/* Arm the EL2 scheduler tick (CNTHV) and enter the first vCPU (no return). */
void vcpu_sched_init(void);
__attribute__((noreturn)) void vcpu_run_first(void);

/* Round-robin world switch, called from the EL2 IRQ handler on a scheduler
 * tick. Saves the running guest's full context, restores the next guest's. */
void vcpu_sched_tick(hyp_trap_frame_t *f);

/* The current guest executed WFI/WFE and is idle. Mark it blocked (so the
 * scheduler skips it) and world-switch to another runnable guest. The blocked
 * guest is woken by vcpu_wake_expired() when its vtimer deadline fires. If no
 * other guest is runnable, this returns and the caller idles via the trap
 * return (the guest re-checks WFI). */
void vcpu_block_current(hyp_trap_frame_t *f);

/* Called on each CNTHP fire: inject the timer IRQ into (and mark runnable) any
 * vCPU whose vtimer deadline has elapsed — including non-current, blocked ones.
 * Returns 1 if the current vCPU should switch (it blocked and another is now
 * runnable), else 0. */
int vcpu_wake_expired(void);

/* CNTHP is shared between the scheduler slice and the running guest's vtimer.
 * hyp_cnthp_arm() programs CNTHP to the sooner of the two deadlines; the
 * vtimer and scheduler both call it after changing their deadline.
 * hyp_sched_deadline() returns the absolute CNTPCT of the next scheduler tick. */
void hyp_cnthp_arm(void);
uint64_t hyp_sched_deadline(void);

#endif /* HYP_VCPU_H */
