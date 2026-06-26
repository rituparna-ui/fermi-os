#ifndef HYP_MMU_EL2_H
#define HYP_MMU_EL2_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal EL2 stage-1 MMU (TTBR0_EL2).
 *
 * WHY (load-bearing for SMP): with the MMU off, every EL2 data access is Normal
 * Non-cacheable. On Non-cacheable memory the architecture does NOT guarantee a
 * global exclusive monitor, and FEAT_LSE atomics (SWP/CAS/LDADD) are likewise
 * only guaranteed inter-PE atomic on Normal Cacheable Inner-Shareable memory
 * (ARM DDI 0487). So an EL2 spinlock on MMU-off memory is NOT inter-PE correct —
 * QEMU TCG happens to make it work (one global monitor regardless of attrs) but
 * real silicon would wedge. To make EL2 spinlocks / atomics correct across
 * physical cores, EL2 must run with its OWN stage-1 MMU mapping its RAM as
 * Normal-WB Inner-Shareable.
 *
 * This is an IDENTITY map (VA == PA, like the rest of the hyp): one L1 table of
 * 512 x 1 GiB block descriptors over a 39-bit VA space (T0SZ=25). GiB 0
 * (0..0x40000000, all MMIO on QEMU virt — GICD/GICR/UART) is Device-nGnRnE+XN;
 * GiB 1..9 (0x40000000..0x280000000, all RAM: guest RAM + the hyp image/pool)
 * is Normal-WB Inner-Shareable, executable (the hyp's own .text lives there).
 *
 * Stage-1 EL2 translates only EL2's own accesses; the guest's accesses still go
 * through its EL1 stage-1 + the per-VM stage-2 (unchanged).
 * ------------------------------------------------------------------------- */

/* Build the EL2 L1 page table (CPU0 only, once, MMU still off). Cleans it to
 * PoC so the cacheable table walker sees it. */
void hyp_mmu_el2_build(void);

/* Program MAIR_EL2/TCR_EL2/TTBR0_EL2 and set SCTLR_EL2.{M,C,I}. Called on EVERY
 * pCPU (CPU0 after build, each secondary at bring-up). Must run before the core
 * touches any lock / shared atomic. */
void hyp_mmu_el2_enable(void);

#endif /* HYP_MMU_EL2_H */
