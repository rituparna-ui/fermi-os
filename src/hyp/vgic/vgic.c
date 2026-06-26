#include "vgic.h"
#include "vcpu.h"
#include "hyp.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * GICv3 virtualization registers. The container toolchain (binutils 2.4x)
 * accepts the symbolic ich_ and icc_sre_el2 mnemonics, so we use them directly.
 *
 * ICH_LR<n>_EL2 layout (per GICv3 / ARM ARM):
 *   State[63:62]  : 0b00 Invalid, 0b01 Pending, 0b10 Active, 0b11 Pending+Active
 *   HW[61]        : 0 = pure virtual (guest EOI does virtual deactivate only)
 *   Group[60]     : 1 = Group1
 *   Priority[55:48]
 *   pINTID[44:32] : physical INTID (only when HW=1) — unused here (HW=0)
 *   vINTID[31:0]  : virtual INTID presented to the guest
 *
 * ICH_VMCR_EL2 seed: VPMR[31:24]=0xFF (allow all priorities), VENG1[1]=1
 *   (virtual Group1 enable), VEOIM[9]=0 (combined drop+deactivate, matching the
 *   guest's default EOImode). => 0xFF000002.
 * ------------------------------------------------------------------------- */

#define ICH_LR_STATE_PENDING (1ULL << 62)
#define ICH_LR_GROUP1        (1ULL << 60)
#define ICH_LR_HW            (1ULL << 61)
#define ICH_LR_PRIO_SHIFT    48
#define ICH_LR_STATE_MASK    (3ULL << 62)

#define ICH_HCR_EN           (1ULL << 0)
/* TC (bit 10) = Trap Common: traps NS-EL1 writes to ICC_SGI0R/SGI1R/ASGI1R_EL1
 * so the hypervisor can software-route inter-processor SGIs to sibling vCPUs
 * (the HW virtual interface only delivers to the resident vCPU). */
#define ICH_HCR_TC           (1ULL << 10)
#define ICC_SRE_EL2_VAL      0xFULL /* Enable|DIB|DFB|SRE */
#define ICH_VMCR_SEED        0xFF000002ULL

static uint32_t vgic_nr_lr; /* number of implemented List Registers (VPL) */

/* Read/write the indexed List Register. The architecture only defines fixed
 * register names, so we switch on the index. We bound everything by vgic_nr_lr,
 * but provide the full 16 cases for completeness. */
static uint64_t lr_read(uint32_t n) {
  uint64_t v = 0;
  switch (n) {
  case 0:  __asm__ __volatile__("mrs %0, ich_lr0_el2"  : "=r"(v)); break;
  case 1:  __asm__ __volatile__("mrs %0, ich_lr1_el2"  : "=r"(v)); break;
  case 2:  __asm__ __volatile__("mrs %0, ich_lr2_el2"  : "=r"(v)); break;
  case 3:  __asm__ __volatile__("mrs %0, ich_lr3_el2"  : "=r"(v)); break;
  case 4:  __asm__ __volatile__("mrs %0, ich_lr4_el2"  : "=r"(v)); break;
  case 5:  __asm__ __volatile__("mrs %0, ich_lr5_el2"  : "=r"(v)); break;
  case 6:  __asm__ __volatile__("mrs %0, ich_lr6_el2"  : "=r"(v)); break;
  case 7:  __asm__ __volatile__("mrs %0, ich_lr7_el2"  : "=r"(v)); break;
  case 8:  __asm__ __volatile__("mrs %0, ich_lr8_el2"  : "=r"(v)); break;
  case 9:  __asm__ __volatile__("mrs %0, ich_lr9_el2"  : "=r"(v)); break;
  case 10: __asm__ __volatile__("mrs %0, ich_lr10_el2" : "=r"(v)); break;
  case 11: __asm__ __volatile__("mrs %0, ich_lr11_el2" : "=r"(v)); break;
  case 12: __asm__ __volatile__("mrs %0, ich_lr12_el2" : "=r"(v)); break;
  case 13: __asm__ __volatile__("mrs %0, ich_lr13_el2" : "=r"(v)); break;
  case 14: __asm__ __volatile__("mrs %0, ich_lr14_el2" : "=r"(v)); break;
  case 15: __asm__ __volatile__("mrs %0, ich_lr15_el2" : "=r"(v)); break;
  }
  return v;
}

static void lr_write(uint32_t n, uint64_t v) {
  switch (n) {
  case 0:  __asm__ __volatile__("msr ich_lr0_el2,  %0" ::"r"(v)); break;
  case 1:  __asm__ __volatile__("msr ich_lr1_el2,  %0" ::"r"(v)); break;
  case 2:  __asm__ __volatile__("msr ich_lr2_el2,  %0" ::"r"(v)); break;
  case 3:  __asm__ __volatile__("msr ich_lr3_el2,  %0" ::"r"(v)); break;
  case 4:  __asm__ __volatile__("msr ich_lr4_el2,  %0" ::"r"(v)); break;
  case 5:  __asm__ __volatile__("msr ich_lr5_el2,  %0" ::"r"(v)); break;
  case 6:  __asm__ __volatile__("msr ich_lr6_el2,  %0" ::"r"(v)); break;
  case 7:  __asm__ __volatile__("msr ich_lr7_el2,  %0" ::"r"(v)); break;
  case 8:  __asm__ __volatile__("msr ich_lr8_el2,  %0" ::"r"(v)); break;
  case 9:  __asm__ __volatile__("msr ich_lr9_el2,  %0" ::"r"(v)); break;
  case 10: __asm__ __volatile__("msr ich_lr10_el2, %0" ::"r"(v)); break;
  case 11: __asm__ __volatile__("msr ich_lr11_el2, %0" ::"r"(v)); break;
  case 12: __asm__ __volatile__("msr ich_lr12_el2, %0" ::"r"(v)); break;
  case 13: __asm__ __volatile__("msr ich_lr13_el2, %0" ::"r"(v)); break;
  case 14: __asm__ __volatile__("msr ich_lr14_el2, %0" ::"r"(v)); break;
  case 15: __asm__ __volatile__("msr ich_lr15_el2, %0" ::"r"(v)); break;
  }
}

void vgic_init(void) {
  /* Enable EL2 access to the GICv3 sysreg interface; let guest ICC_SRE_EL1
   * stick. Write then read back (RAO/WI on some implementations). */
  uint64_t sre = ICC_SRE_EL2_VAL;
  __asm__ __volatile__("msr icc_sre_el2, %0\n\tisb" ::"r"(sre));
  __asm__ __volatile__("mrs %0, icc_sre_el2" : "=r"(sre));

  /* VPL = ICH_VTR_EL2.ListRegs[4:0] + 1. */
  uint64_t vtr;
  __asm__ __volatile__("mrs %0, ich_vtr_el2" : "=r"(vtr));
  vgic_nr_lr = (uint32_t)((vtr & 0x1F) + 1);

  /* Clear the live hardware interface; per-vCPU state is seeded by
   * vgic_vcpu_reset and loaded by vgic_restore on each world entry. */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    lr_write(i, 0);
  }
  __asm__ __volatile__("msr ich_ap0r0_el2, %0" ::"r"(0ULL));
  __asm__ __volatile__("msr ich_ap1r0_el2, %0" ::"r"(0ULL));

  hyp_puts("[VGIC] ICC_SRE_EL2=");
  hyp_puthex(sre);
  hyp_puts(" VPL=");
  hyp_puthex(vgic_nr_lr);
  hyp_puts(" (virtual CPU interface ready)\n");
}

uint32_t vgic_num_lr(void) { return vgic_nr_lr; }

/* Seed a fresh per-vCPU vGIC state: virtual interface enabled, Group1 + PMR
 * seeded, LRs empty, MMIO model zeroed. */
void vgic_vcpu_reset(vcpu_vgic_t *g) {
  /* TC (SGI trapping) is NOT seeded here: it is enabled per-vCPU only for SMP
   * VMs (vgic_enable_sgi_trap), because TC traps the WHOLE "common" ICC group
   * (incl. ICC_PMR_EL1), not just ICC_SGI1R_EL1. A single-vCPU VM has no
   * sibling to SGI, so it keeps TC off and its ICC_* accesses flow straight to
   * the HW virtual interface (no new traps -> no regression). */
  g->hcr = ICH_HCR_EN;
  g->vmcr = ICH_VMCR_SEED;
  g->ap0r0 = 0;
  g->ap1r0 = 0;
  /* Zero only the implemented LRs (vgic_init must run first to set vgic_nr_lr).
   * The lr[] array is statically sized 16 as an upper bound. */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) g->lr[i] = 0;
  g->gicd_ctlr = 0;
  g->gicd_isenabler0 = 0;
  g->gicd_isenabler1 = 0;
  g->gicr_igroupr0 = 0;
  g->gicr_igrpmodr0 = 0;
  g->gicr_isenabler0 = 0;
}

/* Save the live hardware virtual interface into `g` (world exit). */
void vgic_save(vcpu_vgic_t *g) {
  __asm__ __volatile__("mrs %0, ich_vmcr_el2" : "=r"(g->vmcr));
  __asm__ __volatile__("mrs %0, ich_ap0r0_el2" : "=r"(g->ap0r0));
  __asm__ __volatile__("mrs %0, ich_ap1r0_el2" : "=r"(g->ap1r0));
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    g->lr[i] = lr_read(i);
  }
  /* Quiesce the interface while another guest / EL2 runs. */
  __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(0ULL));
}

/* Restore `g` into the live hardware virtual interface (world entry). En last. */
void vgic_restore(const vcpu_vgic_t *g) {
  __asm__ __volatile__("msr ich_vmcr_el2, %0" ::"r"(g->vmcr));
  __asm__ __volatile__("msr ich_ap0r0_el2, %0" ::"r"(g->ap0r0));
  __asm__ __volatile__("msr ich_ap1r0_el2, %0" ::"r"(g->ap1r0));
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    lr_write(i, g->lr[i]);
  }
  __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(g->hcr));
}

/* ---------------------------------------------------------------------------
 * GICD/GICR MMIO emulation (M5).
 *
 * The guest's gic.c touches a small, fixed set of registers (verified against
 * src/exception/gic/gic.h): GICD_CTLR, GICD_ISENABLER; GICR_WAKER, and in the
 * redistributor SGI/PPI frame GICR_IGROUPR0 / GICR_IGRPMODR0 / GICR_ISENABLER0.
 * We model just enough state to make the guest's bring-up succeed; the real
 * distributor is never programmed by the guest once these pages trap.
 * ------------------------------------------------------------------------- */

#define GICD_IPA_BASE 0x08000000ULL
#define GICD_IPA_END  0x08010000ULL /* 64 KiB distributor */
#define GICR_IPA_BASE 0x080A0000ULL
#define GICR_IPA_END  0x080C0000ULL /* RD + SGI frames (128 KiB) */

#define R_GICD_CTLR        0x0000
#define R_GICD_ISENABLER0  0x0100
#define R_GICD_ISENABLER1  0x0104
#define R_GICR_WAKER       0x0014
#define R_GICR_SGI_IGROUPR0   0x10080
#define R_GICR_SGI_IGRPMODR0  0x10D00
#define R_GICR_SGI_ISENABLER0 0x10100

#define GICD_CTLR_ARE_NS  (1U << 4)
#define GICD_CTLR_EN_G1NS (1U << 1)

int vgic_mmio_is_target(uint64_t ipa) {
  return (ipa >= GICD_IPA_BASE && ipa < GICD_IPA_END) ||
         (ipa >= GICR_IPA_BASE && ipa < GICR_IPA_END);
}

void vgic_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                       int size_bytes) {
  vcpu_vgic_t *vd = &cur_vcpu->vgic; /* per-VM distributor/redistributor model */
  uint64_t off;
  if (ipa >= GICR_IPA_BASE) {
    off = ipa - GICR_IPA_BASE;
  } else {
    off = ipa - GICD_IPA_BASE;
  }

  /* Mask to the access width so sub-32-bit accesses don't contribute garbage
   * upper bytes (esp. for the |= read-modify-write of the ISENABLER regs). */
  uint32_t size_mask =
      (size_bytes >= 4) ? 0xFFFFFFFFU : ((1U << (size_bytes * 8)) - 1U);

  if (is_write) {
    uint32_t w = (uint32_t)*val & size_mask;
    switch (off) {
    case R_GICD_CTLR:        vd->gicd_ctlr = w & (GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1NS); break;
    case R_GICD_ISENABLER0:  vd->gicd_isenabler0 |= w; break;
    case R_GICD_ISENABLER1:  vd->gicd_isenabler1 |= w; break;
    case R_GICR_WAKER:       /* ProcessorSleep handled on read; ignore write */ break;
    case R_GICR_SGI_IGROUPR0:   vd->gicr_igroupr0 = w; break;
    case R_GICR_SGI_IGRPMODR0:  vd->gicr_igrpmodr0 = w; break;
    case R_GICR_SGI_ISENABLER0: vd->gicr_isenabler0 |= w; break;
    default: /* unmodelled write — drop silently */ break;
    }
    return;
  }

  /* Read. */
  uint32_t r = 0;
  switch (off) {
  case R_GICD_CTLR:        r = vd->gicd_ctlr; break;
  case R_GICD_ISENABLER0:  r = vd->gicd_isenabler0; break;
  case R_GICD_ISENABLER1:  r = vd->gicd_isenabler1; break;
  case R_GICR_WAKER:       r = 0; /* ProcessorSleep=0, ChildrenAsleep=0 — the
                                   * guest's poll loop (gic.c:30) exits */ break;
  case R_GICR_SGI_IGROUPR0:   r = vd->gicr_igroupr0; break;
  case R_GICR_SGI_IGRPMODR0:  r = vd->gicr_igrpmodr0; break;
  case R_GICR_SGI_ISENABLER0: r = vd->gicr_isenabler0; break;
  default: r = 0; break;
  }
  *val = r & size_mask;
}

/* Compose a pending Group1 List Register value for `intid`. */
static uint64_t lr_pending(uint32_t intid) {
  return ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
         (0xA0ULL << ICH_LR_PRIO_SHIFT) | (uint64_t)intid;
}

void vgic_inject_ppi(uint32_t intid) {
  /* Inject into a free LIVE List Register (current guest). */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    uint64_t lr = lr_read(i);
    if ((lr & ICH_LR_STATE_MASK) == 0) {
      lr_write(i, lr_pending(intid));
      return;
    }
    /* Already pending/active for this INTID — guest hasn't consumed it yet. */
    if ((uint32_t)(lr & 0xFFFFFFFFULL) == intid) {
      return;
    }
  }
  /* No free LR: guest is behind on this periodic IRQ; drop silently. */
}

/* Inject an SPI into the current guest's live List Registers, reporting success.
 * Returns 1 if the INTID was enqueued (or already pending — coalesced), 0 if
 * there was no free LR (the caller may keep it pending) OR the INTID is outside
 * the SPI range. The INTID gate (32..1019) is defense-in-depth so a caller that
 * derives an INTID from guest-programmed data (MSI-X Msg Data) can never enqueue
 * an SGI/PPI (incl. the EL2 timer PPI 26 or guest timer PPI 30) or a reserved
 * INTID into a List Register — it is NOT the only barrier (the MSI-X device also
 * clamps to its own SPI range), but it ensures the LR can only ever hold a SPI.
 * Distinct from vgic_inject_ppi (which stays ungated so the vtimer PPI 30 path
 * is unaffected). */
int vgic_inject_spi_try(uint32_t intid) {
  if (intid < 32 || intid > 1019) {
    return 0; /* reject SGI/PPI/reserved — SPIs only */
  }
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    uint64_t lr = lr_read(i);
    if ((lr & ICH_LR_STATE_MASK) == 0) {
      lr_write(i, lr_pending(intid));
      return 1;
    }
    if ((uint32_t)(lr & 0xFFFFFFFFULL) == intid) {
      return 1; /* already pending/active for this INTID (edge coalesced) */
    }
  }
  return 0; /* no free LR */
}

/* Enable SGI trapping (ICH_HCR_EL2.TC) for THIS vCPU's vGIC state. Only SMP
 * vCPUs set this: TC traps the whole common ICC group (so the hyp can software-
 * route ICC_SGI1R_EL1 to sibling vCPUs), which also catches ICC_PMR_EL1 — hence
 * vgic_emulate_pmr below. Re-applied by vcpu_init_state after every reset. */
void vgic_enable_sgi_trap(struct vcpu_vgic *g) { g->hcr |= ICH_HCR_TC; }

/* Emulate a trapped ICC_PMR_EL1 access. TC traps the common ICC group, of which
 * PMR is a member; forward it to the virtual interface priority mask
 * ICH_VMCR_EL2.VPMR[31:24], updating BOTH the live register and the saved
 * per-vCPU copy so a later world-switch save does not clobber it. */
void vgic_emulate_pmr(struct vcpu_vgic *g, int is_write, uint64_t *val) {
  uint64_t vmcr;
  __asm__ __volatile__("mrs %0, ich_vmcr_el2" : "=r"(vmcr));
  if (is_write) {
    vmcr = (vmcr & ~(0xFFULL << 24)) | ((*val & 0xFFULL) << 24);
    __asm__ __volatile__("msr ich_vmcr_el2, %0\n\tisb" ::"r"(vmcr));
    g->vmcr = vmcr;
  } else {
    *val = (vmcr >> 24) & 0xFFULL;
  }
}

void vgic_inject_to(struct vcpu_vgic *g, uint32_t intid) {
  /* Inject into a non-current vCPU's SAVED LR array; presented when restored. */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    if ((g->lr[i] & ICH_LR_STATE_MASK) == 0) {
      g->lr[i] = lr_pending(intid);
      return;
    }
    if ((uint32_t)(g->lr[i] & 0xFFFFFFFFULL) == intid) {
      return; /* already pending/active for this INTID */
    }
  }
}
