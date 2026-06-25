#ifndef HYP_SYSREGS_H
#define HYP_SYSREGS_H

/* EL2 system-register bit definitions used by the FermiOS type-1 hypervisor.
 *
 * All values are for the NON-VHE EL2 regime (HCR_EL2.E2H = 0). The CPU model
 * is QEMU '-cpu max' on 'virt,gic-version=3,virtualization=on', so FEAT_VHE,
 * stage-2 translation, GICv3 virtualization, SVE, and CNTVOFF all exist.
 *
 * Bit positions and values below were cross-checked against ARM DDI 0487
 * (Armv8-A ARM) and an adversarial architecture review. See src/hyp/README
 * notes inline where a value was corrected away from a "common knowledge"
 * constant that turned out to be wrong.
 */

/* ---------------------------------------------------------------------------
 * HCR_EL2 — Hypervisor Configuration Register
 * ------------------------------------------------------------------------- */
#define HCR_EL2_VM    (1ULL << 0)   /* Stage-2 (virtualization) MMU enable     */
#define HCR_EL2_SWIO  (1ULL << 1)   /* Set/way invalidation override           */
#define HCR_EL2_FMO   (1ULL << 3)   /* Physical FIQ routed to EL2              */
#define HCR_EL2_IMO   (1ULL << 4)   /* Physical IRQ routed to EL2              */
#define HCR_EL2_AMO   (1ULL << 5)   /* Physical SError routed to EL2           */
#define HCR_EL2_TWI   (1ULL << 13)  /* Trap guest WFI to EL2                   */
#define HCR_EL2_TWE   (1ULL << 14)  /* Trap guest WFE to EL2                   */
#define HCR_EL2_TSC   (1ULL << 19)  /* Trap guest SMC to EL2                   */
#define HCR_EL2_TGE   (1ULL << 27)  /* Trap General Exceptions — MUST be 0     */
#define HCR_EL2_HCD   (1ULL << 29)  /* HVC disable — MUST be 0 to allow HVC    */
#define HCR_EL2_RW    (1ULL << 31)  /* Lower EL is AArch64 (guest is 64-bit)   */
#define HCR_EL2_E2H   (1ULL << 34)  /* VHE — MUST be 0 (non-VHE hypervisor)    */

/* Milestone 1: RW only. Guest is AArch64, stage-2 off (IPA==PA passthrough),
 * no physical-interrupt routing to EL2 yet (guest drives the real GIC). */
#define HCR_EL2_M1    (HCR_EL2_RW)

/* ---------------------------------------------------------------------------
 * CPTR_EL2 — Architectural Feature Trap Register (non-VHE, E2H=0)
 *
 * The non-VHE RES1 base is 0x33FF (bits[13:12], bit[9], bits[7:0]). That base
 * INCLUDES TZ (bit8)=1, which would TRAP guest SVE — wrong for us. We must
 * clear both TFP (bit10, FP/SIMD trap) and TZ (bit8, SVE trap) so the guest's
 * NEON (and AAPCS64 varargs in d0-d7) and any SVE run without trapping.
 *   0x33FF & ~(1<<8) & ~(1<<10) = 0x32FF
 * (Trap EC reference: FP/SIMD access trap = EC 0x07; SVE access trap = EC 0x19.)
 * ------------------------------------------------------------------------- */
#define CPTR_EL2_TFP  (1ULL << 10)
#define CPTR_EL2_TZ   (1ULL << 8)
#define CPTR_EL2_RES1 0x33FFULL
#define CPTR_EL2_M1   (CPTR_EL2_RES1 & ~CPTR_EL2_TZ & ~CPTR_EL2_TFP) /* 0x32FF */

/* ---------------------------------------------------------------------------
 * SCTLR_EL2 — System Control Register (EL2)
 *
 * We run EL2 with the MMU and caches OFF (M=C=I=0), alignment-check on stack
 * (SA=1). 0x30C50838 is the well-known AArch64 reset-style RES1 pattern; for
 * SCTLR_EL2 the RES1 set is not bit-identical to SCTLR_EL1, but this value
 * decodes to M=0,A=0,C=0,SA=1,I=0 which is exactly the intent and is accepted
 * by QEMU. Treat as "MMU/caches off, sane defaults".
 * ------------------------------------------------------------------------- */
#define SCTLR_EL2_M   (1ULL << 0)
#define SCTLR_EL2_A   (1ULL << 1)
#define SCTLR_EL2_C   (1ULL << 2)
#define SCTLR_EL2_SA  (1ULL << 3)
#define SCTLR_EL2_I   (1ULL << 12)
#define SCTLR_EL2_RES1_OFF 0x30C50838ULL  /* M=C=I=0, SA=1 */

/* ---------------------------------------------------------------------------
 * MDCR_EL2 — Monitor Debug Configuration Register (EL2)
 *
 * All guest debug/PMU trap bits left 0 so the guest's PMU bring-up (cpu.c
 * writes PMCR_EL0 / PMCNTENSET_EL0) and debug-register reads do not trap.
 * HPMN[4:0] must be set to the number of implemented PMU counters; setting it
 * larger than PMCR_EL0.N is CONSTRAINED UNPREDICTABLE. hyp_boot reads
 * PMCR_EL0.N at runtime and writes that into HPMN.
 * ------------------------------------------------------------------------- */
#define MDCR_EL2_TPMCR (1ULL << 5)
#define MDCR_EL2_TPM   (1ULL << 6)
#define MDCR_EL2_TDE   (1ULL << 8)
#define MDCR_EL2_TDA   (1ULL << 9)
#define MDCR_EL2_TDOSA (1ULL << 10)
#define MDCR_EL2_TDRA  (1ULL << 11)
#define MDCR_EL2_HPMN_MASK 0x1FULL

/* ---------------------------------------------------------------------------
 * CNTHCTL_EL2 — Counter-timer Hypervisor Control (non-VHE, E2H=0)
 *   bit0 EL1PCTEN: 1 = allow EL1/EL0 physical counter (CNTPCT) + CNTFRQ reads
 *   bit1 EL1PCEN : 1 = allow EL1/EL0 physical timer (CNTP_*) access
 * Polarity: 1 = ALLOW (no trap), 0 = TRAP. Reset value is UNKNOWN, so it MUST
 * be programmed explicitly. M1 = 0x3 (full passthrough; guest drives the real
 * physical timer directly). M3 will change EL1PCEN to 0 to trap CNTP_* arms.
 * ------------------------------------------------------------------------- */
#define CNTHCTL_EL2_EL1PCTEN (1ULL << 0)
#define CNTHCTL_EL2_EL1PCEN  (1ULL << 1)
#define CNTHCTL_EL2_M1       (CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN) /* 0x3 */

/* ---------------------------------------------------------------------------
 * SPSR_EL2 — Saved Program Status for the eret into the guest.
 *   M[3:0] = 0b0101 (EL1h: EL1 using SP_EL1)   — NOT EL1t (0b0100)
 *   M[4]   = 0      (AArch64)
 *   DAIF   = 0xF << 6 (all of D,A,I,F masked at guest entry; the guest's
 *                      gic_init / boot path unmasks IRQs itself)
 *   => 0x3C5
 * ------------------------------------------------------------------------- */
#define SPSR_EL2_GUEST_ENTRY 0x3C5ULL

/* Guest entry IPA == KERNEL_PA. The guest ELF links VA 0xFFFF000040000000 but
 * loads AT physical 0x40000000; its _start (boot.S) runs from there. */
#define GUEST_ENTRY_IPA 0x40000000ULL

#endif /* HYP_SYSREGS_H */
