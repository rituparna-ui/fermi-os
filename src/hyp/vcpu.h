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
  uint32_t gicd_isenabler0;
  uint32_t gicr_igroupr0;
  uint32_t gicr_igrpmodr0;
  uint32_t gicr_isenabler0;
} vcpu_vgic_t;

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

  uint64_t vttbr_el2; /* stage-2 base | (VMID << 48) — per-VM address space */
  uint32_t vmid;
  uint32_t id;        /* 0, 1, ... */
  const char *name;
  int runnable;       /* 0 = blocked on WFI awaiting its next interrupt */
  int dead;           /* 1 = powered off (PSCI SYSTEM_OFF), never runs again */

  /* Pristine image, for PSCI SYSTEM_RESET (warm restart). On reset the
   * hypervisor re-copies [img_src, img_src+img_size) to img_dst_pa (the host
   * PA backing the guest's load IPA), then re-initialises register state and
   * re-enters at entry_ipa. */
  const uint8_t *img_src;
  uint64_t       img_dst_pa;
  uint64_t       img_size;
  uint64_t       entry_ipa;
  uint64_t       sp_el1_init;
} vcpu_t;

/* The currently-running vCPU (set by the scheduler before each guest entry).
 * vtimer / vgic emulation use this to find their per-VM state. */
extern vcpu_t *cur_vcpu;

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
