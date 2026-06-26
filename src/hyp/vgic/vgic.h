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

/* Per-vCPU vGIC state lives in vcpu.h; forward-declare so vgic.h does not have
 * to include it (avoids a circular include — vcpu.h pulls in this header's
 * sibling declarations indirectly). */
struct vcpu_vgic;

/* Initialise the EL2 + virtual GIC interface (one-time global setup: ICC_SRE_EL2
 * enable, read VPL). Per-vCPU state is set up by vgic_vcpu_reset. */
void vgic_init(void);

/* Number of implemented List Registers (VPL), read from ICH_VTR_EL2. */
uint32_t vgic_num_lr(void);

/* Per-vCPU vGIC lifecycle for the multi-VM world switch. */
void vgic_vcpu_reset(struct vcpu_vgic *g);
void vgic_save(struct vcpu_vgic *g);      /* live HW interface -> g  (exit)  */
void vgic_restore(const struct vcpu_vgic *g); /* g -> live HW interface (entry) */

/* Inject a virtual PPI/SPI (e.g. timer INTID 30) into the CURRENT guest as a
 * pending Group1 interrupt, written directly to a free live List Register. */
void vgic_inject_ppi(uint32_t intid);

/* Like vgic_inject_ppi but (a) reports whether the INTID was actually enqueued
 * (returns 0 = no free List Register, so the caller may keep it pending) and
 * (b) gates the INTID to the SPI range 32..1019 (defense-in-depth for callers
 * that derive the INTID from guest-supplied data, e.g. MSI-X Msg Data). Use this
 * — NOT vgic_inject_ppi — for any SPI whose number a guest can influence;
 * vgic_inject_ppi stays ungated so the vtimer PPI 30 path is unaffected. */
int vgic_inject_spi_try(uint32_t intid);

/* Inject into a NON-current vCPU's SAVED vGIC state (its lr[] array), so the
 * interrupt is present when that vCPU is next restored. Used to wake a blocked
 * vCPU on its timer deadline while another VM is running. */
void vgic_inject_to(struct vcpu_vgic *g, uint32_t intid);

/* SMP only: enable ICH_HCR_EL2.TC for this vCPU so guest writes to the common
 * ICC group (notably ICC_SGI1R_EL1) trap to EL2 for software SGI routing.
 * Single-vCPU VMs must NOT call this (TC also traps ICC_PMR_EL1 et al.). */
void vgic_enable_sgi_trap(struct vcpu_vgic *g);

/* Emulate a trapped ICC_PMR_EL1 (a side effect of TC trapping the common group)
 * by forwarding to ICH_VMCR_EL2.VPMR. `val` is the source (write) / dest (read).*/
void vgic_emulate_pmr(struct vcpu_vgic *g, int is_write, uint64_t *val);

/* --- M5: GICD/GICR MMIO emulation --------------------------------------- */

/* True if `ipa` falls in the emulated GICD or GICR window. */
int vgic_mmio_is_target(uint64_t ipa);

/* Emulate a trapped GICD/GICR access. `is_write` selects direction; `val`
 * points at the source (write) or destination (read) value, `size_bytes` is
 * the access width (1/2/4/8). The distributor/redistributor state lives in a
 * software model so the guest never touches the real GIC distributor. */
void vgic_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val, int size_bytes);

#endif /* HYP_VGIC_H */
