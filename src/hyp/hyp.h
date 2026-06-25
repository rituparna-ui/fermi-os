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
#define HCR_TID3 (1ULL << 18) /* Trap ID group 3 (ID_AA64*) reads to EL2      */
#define HCR_RW (1ULL << 31)  /* EL1 execution state is AArch64                */

/* CNTHCTL_EL2 bits (non-VHE): let EL1/EL0 reach the physical counter/timer */
#define CNTHCTL_EL1PCTEN (1ULL << 0)
#define CNTHCTL_EL1PCEN (1ULL << 1)

/* Exception-class values we care about in ESR_EL2[31:26]. */
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK 0x3FULL
#define EC_SYSREG 0x18    /* Trapped MSR/MRS/system instruction (AArch64)     */
#define EC_HVC64 0x16     /* HVC instruction execution in AArch64 state       */
#define EC_DABT_LOWER 0x24 /* Data abort from a lower EL (stage-2 fault, etc.) */
#define EC_IABT_LOWER 0x20 /* Instruction abort from a lower EL               */

/* Per-vCPU control block. For Milestone 2 the single guest is co-resident at
 * EL1, so this mostly tracks statistics and holds the slots that a real
 * world-switch (M5) will save/restore. Lives in hypervisor-private memory
 * (.hyp_tables) so the guest cannot see or zero it. */
typedef struct {
  uint64_t id;           /* vCPU identifier                                  */
  uint64_t hvc_count;    /* hypercalls serviced                              */
  uint64_t sysreg_traps; /* emulated MSR/MRS accesses                        */
  uint64_t abort_count;  /* stage-2 / lower-EL aborts seen                   */
  /* Reserved for M5 world-switch context (guest EL1 sysregs). */
  uint64_t sp_el1, elr_el1, spsr_el1;
  uint64_t sctlr_el1, ttbr0_el1, ttbr1_el1, tcr_el1, mair_el1, vbar_el1;
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
