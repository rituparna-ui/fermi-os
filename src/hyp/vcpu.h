#ifndef HYP_VCPU_H
#define HYP_VCPU_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Per-vCPU state for the FermiOS VHE hypervisor (milestone 3: single guest,
 * one vCPU, KVM-style enter/exit that RETURNS to the host C scheduler).
 *
 * Field offsets are consumed by world_switch.S — keep this struct and the
 * VCPU_OFF_* equates there in lockstep. All fields are 8 bytes.
 *
 * VHE banking note: the host runs at EL2 in the EL2&0 regime and never touches
 * the real EL1 sysreg bank, so for a SINGLE guest the guest's EL1 sysregs live
 * undisturbed in the hardware EL1 bank across host execution — no _EL12
 * save/restore is needed here. Only the GPRs (x0-x30), PC (ELR_EL2), PSTATE
 * (SPSR_EL2) and SP_EL1 are world-switched. (FP/ICC/multi-guest come later.)
 * ------------------------------------------------------------------------- */

typedef struct vcpu {
  /* ---- guest GP + return state (offsets 0..) ---- */
  uint64_t x[31];    /* +0   x0..x30                                  */
  uint64_t pc;       /* +248 ELR_EL2 — guest PC to (re)enter at       */
  uint64_t pstate;   /* +256 SPSR_EL2 — guest PSTATE                  */
  uint64_t sp_el1;   /* +264 guest SP_EL1                             */
  uint64_t vttbr;    /* +272 VTTBR_EL2 (stage-2 base | VMID<<48)      */

  /* ---- host save area (written by __guest_enter, read by exit) ---- */
  uint64_t host_sp;   /* +280 host SP at the call to __guest_enter    */
  uint64_t host_x30;  /* +288 host return address (LR)                */

  /* ---- exit information (written by guest_exit_common) ---- */
  uint64_t exit_reason; /* +296 HYP_EXC_* (sync/irq/fiq/serror)       */
  uint64_t esr;         /* +304 ESR_EL2 at exit                       */
  uint64_t far;         /* +312 FAR_EL2 at exit                       */
  uint64_t hpfar;       /* +320 HPFAR_EL2 at exit                     */
  uint64_t hcr_extra;   /* +328 extra HCR_EL2 bits OR'd in on entry   */
                        /*      (e.g. IMO for interrupt time-slicing) */

  /* ---- bookkeeping (not touched by asm) ---- */
  uint32_t vmid;
  uint32_t id;
  const char *name;
} vcpu_t;

/* Enter the guest described by `v`. Returns when the guest traps to EL2; on
 * return, v->exit_reason / v->esr / v->pc etc. describe the exit, and the host
 * scheduler regains control. Implemented in world_switch.S. */
void vcpu_enter(vcpu_t *v);

#endif /* HYP_VCPU_H */
