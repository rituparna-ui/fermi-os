#ifndef HYP_VCPU_H
#define HYP_VCPU_H

#include <stdint.h>
#include "vuart/vuart.h"

/* ---------------------------------------------------------------------------
 * Per-vCPU state for the FermiOS VHE hypervisor.
 *
 * The leading fields (through hcr_extra) have fixed offsets consumed by
 * world_switch.S — keep this struct and the VCPU_* equates there in lockstep.
 *
 * VHE banking note: the host runs at EL2 in the EL2&0 regime and never touches
 * the real EL1 sysreg bank. For a SINGLE guest the guest's EL1 sysregs live
 * undisturbed in the hardware EL1 bank, so no save/restore is needed. For
 * MULTIPLE guests (M9), switching from guest A to B must save A's EL1 bank and
 * restore B's — done via the _EL12 aliases (vcpu_save_state/vcpu_restore_state),
 * plus FP (q0-q31) and the vGIC virtual-interface state. These extended fields
 * live AFTER the asm-visible block and are only touched by C / by the dedicated
 * save/restore asm that takes their address explicitly.
 * ------------------------------------------------------------------------- */

/* EL1 system + exception register bank (guest-owned; saved/restored via the
 * _EL12 aliases by vcpu_save_state/vcpu_restore_state in vcpu_context.S). */
typedef struct vcpu_el1 {
  uint64_t sctlr, cpacr, ttbr0, ttbr1, tcr, mair, amair, vbar;
  uint64_t contextidr, tpidr_el0, tpidrro_el0, tpidr_el1;
  uint64_t sp_el0, elr_el1, spsr_el1, esr_el1, far_el1, par_el1;
  /* The guest uses the EL1 PHYSICAL timer (CNTP) via passthrough; two guests
   * share the one CNTP, so its compare/control are context-switched here.
   * cntkctl_el1 is a guest EL1 reg (via _EL12). */
  uint64_t cntp_cval, cntp_ctl, cntkctl;
} vcpu_el1_t;

/* FP/SIMD bank: q0..q31 (512B) + FPSR + FPCR. 16-byte aligned for q stores. */
typedef struct vcpu_fp {
  uint8_t  q[32 * 16];
  uint64_t fpsr, fpcr;
} __attribute__((aligned(16))) vcpu_fp_t;

/* Per-vCPU exit accounting (observability / introspection). Bucketed by the
 * class of trap that brought the guest to EL2. */
typedef struct vcpu_stats {
  uint64_t exits;  /* total guest exits         */
  uint64_t hvc;    /* HVC hypercalls (EC 0x16)  */
  uint64_t mmio;   /* stage-2 data aborts to emulated MMIO (EC 0x24) */
  uint64_t irq;    /* physical IRQ exits        */
  uint64_t fault;  /* unhandled/fatal exits     */
  uint64_t other;  /* anything else             */
} vcpu_stats_t;

/* vGIC virtual-interface state (per-vCPU; the ICH_* regs are shared HW). */
typedef struct vcpu_vgic {
  uint64_t hcr, vmcr, ap0r0, ap1r0;
  uint64_t lr[16];
  /* GICD/GICR MMIO software model (per guest). */
  uint32_t gicd_ctlr, gicd_isenabler0;
  uint32_t gicr_igroupr0, gicr_igrpmodr0, gicr_isenabler0;
} vcpu_vgic_t;

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
  uint64_t mpidr;       /* +336 VMPIDR_EL2 — this vCPU's virtual MPIDR */

  /* ---- extended per-vCPU state (M9; not referenced by world_switch.S) ---- */
  vcpu_el1_t  el1;   /* guest EL1 sysreg bank (via _EL12)             */
  vcpu_fp_t   fp;    /* q0-q31 + FPSR/FPCR                            */
  vcpu_vgic_t vgic;  /* virtual GIC interface state                   */
  vuart_t     vuart; /* virtual PL011 console                         */
  vcpu_stats_t stats; /* per-vCPU exit accounting                     */

  /* ---- bookkeeping (not touched by asm) ---- */
  uint32_t vmid;
  uint32_t id;
  const char *name;
} vcpu_t;

/* Enter the guest described by `v`. Returns when the guest traps to EL2; on
 * return, v->exit_reason / v->esr / v->pc etc. describe the exit, and the host
 * scheduler regains control. Implemented in world_switch.S. */
void vcpu_enter(vcpu_t *v);

/* Account one just-returned exit into v->stats, bucketed by class. Call once
 * immediately after vcpu_enter(). (Defined in hyp.c.) */
void vcpu_stat_account(vcpu_t *v);

/* Print a virsh-style per-VM exit summary. (Defined in hyp.c.) */
void vcpu_stats_dump(const vcpu_t *v);

/* Save the live guest EL1 sysreg bank (via _EL12) + FP into the structs, and
 * restore them. Used by the multi-guest scheduler around a world switch.
 * Implemented in vcpu_context.S. */
void vcpu_save_el1(vcpu_el1_t *s);
void vcpu_restore_el1(const vcpu_el1_t *s);
void vcpu_save_fp(vcpu_fp_t *f);
void vcpu_restore_fp(const vcpu_fp_t *f);

#endif /* HYP_VCPU_H */
