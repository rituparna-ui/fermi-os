#ifndef HYP_VGIC_H
#define HYP_VGIC_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Virtual GICv3 for the FermiOS VHE hypervisor (milestone 5, single guest).
 *
 * Hardware property we lean on: with HCR_EL2.IMO=1 and the guest at EL1, the
 * guest ICC_x_EL1 CPU-interface accesses (SRE/PMR/IGRPEN1/IAR1/EOIR1) are
 * transparently redirected by hardware to the VIRTUAL interface (ICV/ICH regs),
 * and ack/EOI are serviced by the List Registers - NO per-ack/EOI trap to EL2.
 *
 * So the hypervisor only:
 *   - enables EL2 + virtual CPU interface (ICC_SRE_EL2, ICH_HCR_EL2.En),
 *   - seeds ICH_VMCR_EL2,
 *   - trap-emulates the small set of GICD/GICR MMIO registers the guest's
 *     gic.c touches (those windows are left stage-2-unmapped),
 *   - injects an interrupt by writing a free List Register (ICH_LR<n>_EL2).
 *
 * Single guest => the virtual interface stays live in hardware between exits,
 * so no per-exit vGIC save/restore (that arrives with multi-guest, M9). The
 * GICD/GICR software model is module-static for the one guest.
 * ------------------------------------------------------------------------- */

/* One-time global init: enable ICC_SRE_EL2, read VPL from ICH_VTR_EL2, clear
 * and enable the virtual interface, seed ICH_VMCR_EL2. */
void vgic_init(void);

/* Number of implemented List Registers (VPL). */
uint32_t vgic_num_lr(void);

/* True if `ipa` is in the emulated GICD or GICR window. */
int vgic_mmio_is_target(uint64_t ipa);

/* Emulate a trapped GICD/GICR access. is_write selects direction; *val is the
 * source (write) or destination (read); size_bytes is 1/2/4/8. */
void vgic_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val, int size_bytes);

/* Inject a pending Group1 virtual interrupt (e.g. timer INTID 30) into the
 * live virtual interface via a free List Register. */
void vgic_inject_ppi(uint32_t intid);

#endif /* HYP_VGIC_H */
