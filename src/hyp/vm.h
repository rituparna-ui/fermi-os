#ifndef HYP_VM_H
#define HYP_VM_H

/* ---------------------------------------------------------------------------
 * EL2 world-switch trap frame + exception decoding.
 *
 * The frame is built on SP_EL2 by hyp_vectors.S. Because EL1/EL0 have banked
 * ELR/SPSR/SP, the hypervisor running at EL2h cannot clobber the guest's
 * ELR_EL1/SPSR_EL1/SP_EL1/SP_EL0 just by taking an exception — so for a single
 * guest the world-switch set is tiny: the guest GPRs x0-x30 plus the EL2-banked
 * syndrome/return registers. The hyp is built -mgeneral-regs-only so no FP/SIMD
 * state needs saving.
 *
 * Layout MUST match the stores in hyp_vectors.S:
 *   [sp + 0]   x0..x30   (31 * 8 = 248 bytes)
 *   [sp + 248] ELR_EL2
 *   [sp + 256] SPSR_EL2
 *   [sp + 264] ESR_EL2
 *   [sp + 272] HPFAR_EL2
 *   [sp + 280] FAR_EL2
 *   Total: 288 bytes (16-byte aligned).
 * ------------------------------------------------------------------------- */
#define HYP_FRAME_SIZE 288

/* Exception "type" passed by the vector stubs (which of the 4 entries fired).
 * Defined outside the __ASSEMBLER__ guard — hyp_vectors.S uses them. */
#define HYP_EXC_SYNC   0
#define HYP_EXC_IRQ    1
#define HYP_EXC_FIQ    2
#define HYP_EXC_SERROR 3

#ifndef __ASSEMBLER__
#include <stdint.h>
typedef struct hyp_trap_frame {
  uint64_t regs[31]; /* x0..x30 */
  uint64_t elr;      /* ELR_EL2  */
  uint64_t spsr;     /* SPSR_EL2 */
  uint64_t esr;      /* ESR_EL2  */
  uint64_t hpfar;    /* HPFAR_EL2 */
  uint64_t far;      /* FAR_EL2  */
} hyp_trap_frame_t;

/* ESR_EL2 exception class (bits[31:26]). */
#define ESR_EC(esr) (((esr) >> 26) & 0x3F)
#define ESR_IL(esr) (((esr) >> 25) & 0x1) /* 1 = 32-bit instr (AArch64 = 1) */

#define EC_WF_TRAPPED   0x01 /* WFI/WFE (HCR_EL2.TWI/TWE)             */
#define EC_TRAPPED_SYSREG 0x18 /* trapped MSR/MRS (CNTP_x / ICC_x)     */
#define EC_HVC_AARCH64  0x16 /* HVC from AArch64 (PSCI / hypercall)   */
#define EC_SMC_AARCH64  0x17 /* SMC (HCR_EL2.TSC)                     */
#define EC_INST_ABORT_LO 0x20 /* instruction abort from lower EL (S2) */
#define EC_DATA_ABORT_LO 0x24 /* data abort from lower EL (stage-2)   */

/* ESR_EL2 ISS for data abort (EC 0x20/0x24). */
#define ISS_DFSC(esr)  ((esr) & 0x3F)
#define ISS_WNR(esr)   (((esr) >> 6) & 0x1)
#define ISS_S1PTW(esr) (((esr) >> 7) & 0x1)
#define ISS_ISV(esr)   (((esr) >> 24) & 0x1)
#define ISS_SAS(esr)   (((esr) >> 22) & 0x3) /* access size: 0=B,1=H,2=W,3=D */
#define ISS_SRT(esr)   (((esr) >> 16) & 0x1F) /* transfer register index     */
#define ISS_SF(esr)    (((esr) >> 15) & 0x1)

/* ESR_EL2 ISS for trapped sysreg (EC 0x18). */
#define ISS_SYS_OP0(esr) (((esr) >> 20) & 0x3)
#define ISS_SYS_OP2(esr) (((esr) >> 17) & 0x7)
#define ISS_SYS_OP1(esr) (((esr) >> 14) & 0x7)
#define ISS_SYS_CRN(esr) (((esr) >> 10) & 0xF)
#define ISS_SYS_RT(esr)  (((esr) >> 5) & 0x1F)
#define ISS_SYS_CRM(esr) (((esr) >> 1) & 0xF)
#define ISS_SYS_DIR(esr) ((esr) & 0x1) /* 0 = write (MSR), 1 = read (MRS) */

/* ESR_EL2 ISS for WFx (EC 0x01): bit0 TI, 0 = WFI, 1 = WFE. */
#define ISS_WFX_TI(esr) ((esr) & 0x1)

/* --- Fermi hypercalls (vendor HVC ids in x0, outside the PSCI range) --- */
#define HVC_FERMI_YIELD    0xFE110000ULL /* yield rest of time slice          */
#define HVC_FERMI_DOORBELL 0xFE110001ULL /* notify peer VM (inject doorbell IRQ) */
#define HVC_FERMI_VMCTL    0xFE110002ULL /* management op (privileged "dom0" VM)
                                          *   x1 = op, x2 = target vCPU id,
                                          *   x3 = arg/buffer IPA (op-specific).
                                          *   ret in x0. */
#define HVC_FERMI_LOG      0xFE110003ULL /* PV console: x1 = buf IPA, x2 = len.
                                          * Hyp prints it tagged with VM name.
                                          * Any VM may call (non-privileged). */
#define HVC_FERMI_WDOG     0xFE110004ULL /* liveness watchdog: x1 = timeout in
                                          * CNTPCT ticks (0 disarms). Arm/pet;
                                          * if not re-called before the deadline
                                          * the hyp reboots this VM. */

/* VMCTL operations (in x1). Results return in registers — no shared buffer, so
 * no IPA translation is needed. */
#define VMCTL_COUNT   0  /* -> x0 = number of vCPUs                           */
#define VMCTL_STATE   1  /* x2=id; -> x0 = packed state (see VMCTL_ST_* below) */
#define VMCTL_RUNS    2  /* x2=id; -> x0 = that VM's run_count                 */
#define VMCTL_RESET   3  /* x2=id; warm-reset that VM (reload image, restart)  */
#define VMCTL_STOP    4  /* x2=id; pause that VM (mark not-runnable)           */
#define VMCTL_START   5  /* x2=id; resume that VM (mark runnable)              */
#define VMCTL_STAT    6  /* x2=id, x3=stat index; -> x0 = that exit counter    */
#define VMCTL_WEIGHT  7  /* x2=id, x3=weight; set proportional CPU share        */
#define VMCTL_CPUTIME 8  /* x2=id; -> x0 = CPU ticks consumed (CNTPCT)          */
#define VMCTL_SNAPSHOT 9 /* x2=id; checkpoint that VM's full state to a slot    */
#define VMCTL_RESTORE 10 /* x2=id; roll that VM back to its snapshot            */
#define VMCTL_MIGRATE 11 /* x2=dst id; clone the snapshot into a DIFFERENT VM   */

/* VMCTL_STAT indices (x3). */
#define VMSTAT_HVC        0
#define VMSTAT_DATA_ABORT 1
#define VMSTAT_SYSREG     2
#define VMSTAT_WFX        3
#define VMSTAT_IRQ        4
#define VMSTAT_NR         5

/* VMCTL_STATE packed result in x0: bit0 runnable, bit1 dead, bits[15:8] vmid. */
#define VMCTL_ST_RUNNABLE 0x1ULL
#define VMCTL_ST_DEAD     0x2ULL
#define VMCTL_ST_VMID(x)  (((x) >> 8) & 0xFF)

#define VMCTL_OK       0
#define VMCTL_EPERM   (-1) /* caller not privileged */
#define VMCTL_EINVAL  (-2) /* bad target id / op    */

/* Doorbell virtual interrupt INTID injected into the notified VM. An SPI
 * (>= 32) so it does not collide with the timer PPI (30). */
#define DOORBELL_INTID 40

/* --- PSCI (the guest's reboot path issues hvc with these in x0) --- */
#define PSCI_VERSION_FN    0x84000000ULL
#define PSCI_CPU_OFF_FN    0x84000002ULL
#define PSCI_SYSTEM_OFF_FN 0x84000008ULL
#define PSCI_SYSTEM_RESET_FN 0x84000009ULL
#define PSCI_CPU_ON_FN64   0xC4000003ULL
#define PSCI_AFFINITY_INFO_FN64 0xC4000004ULL
#define PSCI_FEATURES_FN   0x8400000AULL

#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMETERS (-2)
#define PSCI_DENIED             (-3)
#define PSCI_ALREADY_ON         (-4)

/* C entry from hyp_vectors.S. */
void hyp_dispatch(uint64_t type, hyp_trap_frame_t *f);

#endif /* __ASSEMBLER__ */
#endif /* HYP_VM_H */
