#ifndef HYP_VGIC_H
#define HYP_VGIC_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Virtual GICv3 — interrupt injection via the hardware GICv3 virtualization
 * extension (ICH_* EL2 registers + List Registers).
 *
 * Key hardware behaviour we lean on: with HCR_EL2.IMO=1 and the guest at EL1,
 * the guest's ICC_*_EL1 CPU-interface accesses (SRE/PMR/IGRPEN1/IAR1/EOIR1)
 * are TRANSPARENTLY redirected by hardware to the VIRTUAL interface
 * (the ICV_x / ICH_VMCR_EL2 regs). So the guest's own gic_init configures its
 * virtual CPU
 * interface, and its ICC_IAR1_EL1/ICC_EOIR1_EL1 during IRQ handling are
 * serviced by the List Registers in hardware — NO per-ack/EOI trap to EL2.
 *
 * The hypervisor's job is therefore minimal for a single guest:
 *   - enable EL2 access to the GICv3 sysreg interface (ICC_SRE_EL2)
 *   - enable the virtual CPU interface (ICH_HCR_EL2.En=1)
 *   - seed a sane virtual control state (ICH_VMCR_EL2)
 *   - inject an interrupt by writing a free List Register (ICH_LR<n>_EL2)
 * No per-exit vGIC context save/restore is needed (one guest, interface stays
 * live in hardware between exits).
 * ------------------------------------------------------------------------- */

/* Initialise the EL2 + virtual GIC interface. Reads VPL from ICH_VTR_EL2. */
void vgic_init(void);

/* Inject a virtual PPI/SPI (e.g. timer INTID 30) into the guest as a pending
 * Group1 interrupt. Picks a free List Register; if none is free (a previous
 * injection of the same INTID is still pending/active), it is a no-op (the
 * guest is simply behind and will catch up). */
void vgic_inject_ppi(uint32_t intid);

/* --- M5: GICD/GICR MMIO emulation --------------------------------------- */

/* True if `ipa` falls in the emulated GICD or GICR window. */
int vgic_mmio_is_target(uint64_t ipa);

/* Emulate a trapped GICD/GICR access. `is_write` selects direction; `val`
 * points at the source (write) or destination (read) value, `size_bytes` is
 * the access width (1/2/4/8). The distributor/redistributor state lives in a
 * software model so the guest never touches the real GIC distributor. */
void vgic_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val, int size_bytes);

#endif /* HYP_VGIC_H */
