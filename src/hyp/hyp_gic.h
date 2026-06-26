#ifndef HYP_GIC_H
#define HYP_GIC_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Hypervisor-side (EL2) physical GIC bring-up.
 *
 * Distinct from the vGIC (which presents a VIRTUAL interface to the guest):
 * this configures the hypervisor's OWN physical GICv3 CPU interface so that
 * physical interrupts routed to EL2 (HCR_EL2.IMO=1) — specifically the EL2
 * physical-timer PPI (INTID 26) used by the virtual timer — are actually
 * delivered to EL2 code.
 *
 * GICv3 layout on QEMU virt (confirmed via DTB): GICD @ 0x08000000,
 * GICR @ 0x080A0000, redistributor SGI/PPI frame @ GICR + 0x10000.
 * ------------------------------------------------------------------------- */

#define HYP_GICD_BASE 0x08000000ULL
#define HYP_GICR_BASE 0x080A0000ULL
#define HYP_GICR_SGI_BASE (HYP_GICR_BASE + 0x10000ULL)

/* Bring up the EL2 physical CPU interface (ICC_SRE_EL2 is done by vgic_init;
 * here we set PMR/IGRPEN1 at EL2 and enable + route the CNTHP PPI 26). On the
 * uniprocessor this did everything; under SMP it is the GLOBAL distributor part
 * + pCPU0's own per-core part. */
void hyp_gic_init(void);

/* SMP: per-core EL2 GIC bring-up for a secondary pCPU. Discovers this core's
 * redistributor frame (GICR_TYPER affinity match), wakes it, marks SGIs/PPIs
 * Group1, sets ICC_PMR/IGRPEN1 at EL2. `enable_cnthp` gates PPI 26 (CNTHP) — a
 * secondary that does not yet schedule (Phase A/B idle) MUST leave it masked, or
 * it would take a CNTHP with no current vCPU. Returns the discovered GICR base. */
uint64_t hyp_gic_percpu_init(int enable_cnthp);

/* Acknowledge / EOI a physical IRQ at the EL2 host interface. */
uint32_t hyp_gic_ack(void);
void hyp_gic_eoi(uint32_t intid);

#endif /* HYP_GIC_H */
