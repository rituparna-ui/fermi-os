#ifndef HYP_VTIMER_H
#define HYP_VTIMER_H

#include <stdint.h>
#include "vm.h"

/* ---------------------------------------------------------------------------
 * Virtual EL1 physical timer.
 *
 * The guest drives the EL1 PHYSICAL timer (CNTP_CTL_EL0 / CNTP_CVAL_EL0 /
 * CNTP_TVAL_EL0 / CNTPCT_EL0) and expects its interrupt as GIC PPI INTID 30.
 *
 * Pure pass-through CANNOT work: the EL1 physical timer output is LEVEL-
 * triggered, so after EL2 EOIs the host PPI the comparator is still asserting
 * and storms EL2. The correct design (this module):
 *
 *   - Trap the guest's CNTP_* accesses to EL2 (CNTHCTL_EL2.EL1PCEN=0). The
 *     guest's CNTPCT_EL0 reads stay UNtrapped (EL1PCTEN=1) so time reads are
 *     cheap and the guest's clock matches real time.
 *   - The hypervisor keeps a shadow of the guest's CNTP_CTL/CNTP_CVAL and arms
 *     the EL2 PHYSICAL timer (CNTHP_*_EL2, host PPI INTID 26) to the guest's
 *     deadline.
 *   - When CNTHP fires at EL2 (PPI 26 routed via HCR_EL2.IMO=1), EL2 injects a
 *     virtual INTID 30 into the guest via the vGIC List Registers and disarms
 *     CNTHP. The guest services its IRQ and re-arms CNTP_CVAL_EL0 — which traps
 *     back to EL2 and re-arms CNTHP. No storm, guest's timer.c unchanged.
 * ------------------------------------------------------------------------- */

#define VTIMER_GUEST_PPI 30 /* EL1 physical timer PPI the guest expects   */
#define HYP_CNTHP_PPI    26 /* EL2 physical timer PPI taken at EL2         */

/* Program CNTHCTL_EL2 to trap CNTP_* but allow CNTPCT reads. Called at EL2 boot
 * (overrides the M1/M2 passthrough value). */
void vtimer_init(void);

/* Emulate a trapped guest CNTP_* access (ESR_EL2.EC == 0x18). Returns 1 if the
 * sysreg was a timer register and was handled (caller advances ELR_EL2). */
int vtimer_emulate_sysreg(hyp_trap_frame_t *f);

/* Called from the EL2 IRQ handler when the host CNTHP PPI (26) fires. Injects
 * vINTID 30 into the guest and disarms the EL2 timer until the guest re-arms. */
void vtimer_handle_host_irq(void);

#endif /* HYP_VTIMER_H */
