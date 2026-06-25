#ifndef HYP_HYP_H
#define HYP_HYP_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * hyp.h — Fermi EL2 Type-1 hypervisor (Milestone 1)
 *
 * Boot order (see boot.S): QEMU enters the image at EL2 when the machine is
 * started with `virtualization=on`. boot.S detects EL2, calls hyp_init() to
 * configure the hypervisor, then `eret`s down to EL1 where the existing Fermi
 * kernel continues to run — now as a stage-2-translated guest.
 *
 * Everything here runs at EL2 with the EL2 MMU OFF. All symbol references are
 * PC-relative (adrp/add, -fno-pic), so taking the address of a static object
 * pre-MMU yields its *physical* address — exactly what VTTBR_EL2 / VBAR_EL2
 * need. This mirrors how early_init() runs before the stage-1 MMU is enabled.
 * --------------------------------------------------------------------------- */

/* Stage-2 (VMSAv8-64) descriptor bits — note these differ from stage-1:
 *   - There is no MAIR indirection; MemAttr[5:2] encodes the type directly.
 *   - Access permission is S2AP[7:6] (not AP[2:1]).
 *   - Execute permission is XN[54:53] (left 0 = executable). */
#define S2_VALID (1ULL << 0)
#define S2_TABLE (3ULL << 0)      /* bits[1:0]=11 : table (L0/L1) or page (L3) */
/* A block descriptor at L1/L2 just has bit[0]=1, bit[1]=0 (i.e. S2_VALID).  */
#define S2_AF (1ULL << 10)        /* Access flag */
#define S2_SH_INNER (3ULL << 8)   /* Inner shareable */
#define S2_AP_RW (3ULL << 6)      /* S2AP = read/write at EL0 & EL1 */
#define S2_MEM_NORMAL (0xFULL << 2) /* MemAttr = Normal Inner+Outer WB        */
#define S2_MEM_DEVICE (0x0ULL << 2) /* MemAttr = Device-nGnRnE                */

/* HCR_EL2 bits */
#define HCR_VM (1ULL << 0)   /* Enable stage-2 translation for EL1&0          */
#define HCR_IMO (1ULL << 4)  /* Route physical IRQ to EL2 + enable vIRQ        */
#define HCR_TID3 (1ULL << 18) /* Trap ID group 3 (ID_AA64*) reads to EL2      */
#define HCR_TWI (1ULL << 13)  /* Trap EL0/EL1 WFI to EL2 (idle guest -> yield)  */
#define HCR_RW (1ULL << 31)  /* EL1 execution state is AArch64                */

/* GICv3 EL2 control bits (System Register interface / virtual CPU interface) */
#define ICC_SRE_SRE (1ULL << 0)    /* System Register interface enable        */
#define ICC_SRE_ENABLE (1ULL << 3) /* allow lower-EL ICC_SRE access (no trap) */
#define ICC_CTLR_EOIMODE (1ULL << 1) /* EOIR1 = priority drop only (no deact) */
#define ICH_HCR_EN (1ULL << 0)       /* enable the virtual CPU interface       */
/* ICH_LR<n>_EL2 list-register fields */
#define ICH_LR_GROUP1 (1ULL << 60)
#define ICH_LR_HW (1ULL << 61)
#define ICH_LR_STATE_PENDING (1ULL << 62) /* State[63:62] = 0b01 */
#define ICH_LR_PINTID_SHIFT 32
#define ICH_LR_PRIO_SHIFT 48

/* CNTHCTL_EL2 bits (non-VHE): let EL1/EL0 reach the physical counter/timer */
#define CNTHCTL_EL1PCTEN (1ULL << 0)
#define CNTHCTL_EL1PCEN (1ULL << 1)

/* Exception-class values we care about in ESR_EL2[31:26]. */
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK 0x3FULL
#define EC_WFx 0x01       /* Trapped WFI/WFE                                  */
#define EC_SYSREG 0x18    /* Trapped MSR/MRS/system instruction (AArch64)     */
#define EC_HVC64 0x16     /* HVC instruction execution in AArch64 state       */
#define EC_DABT_LOWER 0x24 /* Data abort from a lower EL (stage-2 fault, etc.) */
#define EC_IABT_LOWER 0x20 /* Instruction abort from a lower EL               */

/* vCPU lifecycle state. */
#define VCPU_UNUSED 0
#define VCPU_READY 1
#define VCPU_RUNNING 2

#define NUM_VCPUS 3

/* Per-vCPU control block: everything needed to suspend a guest at EL2 and
 * later resume it. Lives in hypervisor-private memory (.hyp_tables) so guests
 * can neither see nor zero it. The GP regs come from / go to the EL2 trap
 * frame; PC/PSTATE are ELR_EL2/SPSR_EL2; the EL1 system registers are saved
 * from / restored to the live CPU on each world switch. The physical timer
 * (CNTP_*) is deliberately NOT context-switched here — it stays owned by the
 * primary guest, and its IRQs are injected as they arrive. */
typedef struct {
  uint64_t id;
  int state;

  /* Statistics. */
  uint64_t hvc_count;
  uint64_t sysreg_traps;
  uint64_t abort_count;
  uint64_t virq_injected;
  uint64_t mmio_emulated;

  /* Saved execution state. */
  uint64_t regs[31]; /* x0..x30                              */
  uint64_t pc;       /* resume PC   (ELR_EL2)                */
  uint64_t pstate;   /* resume PSTATE (SPSR_EL2)             */
  uint64_t vttbr;    /* stage-2 base | (VMID << 48)          */

  /* Saved EL1 system-register context. */
  uint64_t sp_el1, sp_el0, elr_el1, spsr_el1;
  uint64_t sctlr_el1, cpacr_el1;
  uint64_t ttbr0_el1, ttbr1_el1, tcr_el1, mair_el1, amair_el1;
  uint64_t vbar_el1, contextidr_el1;
  uint64_t tpidr_el1, tpidrro_el0, tpidr_el0;
  uint64_t esr_el1, far_el1, par_el1;

  /* Per-guest vGIC (virtual CPU interface) state. */
  uint64_t ich_lr[2];   /* list registers 0..1 (pending/active vIRQs)        */
  uint64_t ich_vmcr;    /* virtual machine control (group enables, vPMR)     */
  uint64_t ich_ap1r0;   /* group-1 active priorities                          */

  /* FP/SIMD state: q0..q31 (16 bytes each = 64 u64) plus status/control. */
  uint64_t vregs[64];
  uint64_t fpsr, fpcr;
} vcpu_t;

/* Configure EL2 and stage-2, install the EL2 vector table. Called once from
 * boot.S while still at EL2, MMU off. boot.S performs the eret to EL1. */
void hyp_init(void);

/* Minimal trap frame pushed by the EL2 vector stubs (x0..x30). */
typedef struct {
  uint64_t x[31];
} el2_frame_t;

/* C dispatcher for EL2 exceptions. `index` is the vector slot (0..15);
 * 8 = sync from a lower EL (AArch64), which is where guest HVC/aborts land. */
void el2_dispatch(uint64_t index, el2_frame_t *frame);

#endif /* HYP_HYP_H */
