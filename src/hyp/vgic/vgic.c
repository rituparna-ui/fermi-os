#include "vgic.h"
#include "uart/uart.h"

/* GICv3 virtualization: the binutils in osdev:dev accepts the symbolic ich_*
 * and icc_sre_el2 mnemonics, so we use them directly.
 *
 * ICH_LR<n>_EL2 layout:
 *   State[63:62] 0=Invalid 1=Pending 2=Active 3=Pending+Active
 *   HW[61]       0 = pure virtual (guest EOI does virtual deactivate only)
 *   Group[60]    1 = Group1
 *   Priority[55:48]
 *   vINTID[31:0] virtual INTID presented to the guest
 *
 * ICH_VMCR_EL2 seed: VPMR[31:24]=0xFF (allow all prios), VENG1[1]=1 (virtual
 * Group1 enable), VEOIM[9]=0 (combined drop+deactivate, matches FermiOS's
 * single ICC_EOIR1_EL1 in gic_end_irq). => 0xFF000002. */

#define ICH_LR_STATE_PENDING (1ULL << 62)
#define ICH_LR_GROUP1        (1ULL << 60)
#define ICH_LR_STATE_MASK    (3ULL << 62)
#define ICH_LR_PRIO_SHIFT    48

#define ICH_HCR_EN      (1ULL << 0)
#define ICC_SRE_EL2_VAL 0xFULL      /* Enable|DIB|DFB|SRE */
#define ICH_VMCR_SEED   0xFF000002ULL

static uint32_t vgic_nr_lr;

/* GICD/GICR software model for the single guest. */
static struct {
  uint32_t gicd_ctlr;
  uint32_t gicd_isenabler0;
  uint32_t gicr_igroupr0;
  uint32_t gicr_igrpmodr0;
  uint32_t gicr_isenabler0;
} vd;

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
  uint64_t sre = ICC_SRE_EL2_VAL;
  __asm__ __volatile__("msr icc_sre_el2, %0\n\tisb" ::"r"(sre));
  __asm__ __volatile__("mrs %0, icc_sre_el2" : "=r"(sre));

  uint64_t vtr;
  __asm__ __volatile__("mrs %0, ich_vtr_el2" : "=r"(vtr));
  vgic_nr_lr = (uint32_t)((vtr & 0x1F) + 1);

  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    lr_write(i, 0);
  }
  __asm__ __volatile__("msr ich_ap0r0_el2, %0" ::"r"(0ULL));
  __asm__ __volatile__("msr ich_ap1r0_el2, %0" ::"r"(0ULL));
  __asm__ __volatile__("msr ich_vmcr_el2, %0" ::"r"(ICH_VMCR_SEED));
  __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(ICH_HCR_EN));

  uart_printf("[VGIC] ICC_SRE_EL2=%x VPL=%u (virtual CPU interface ready)\n",
              sre, (uint64_t)vgic_nr_lr);
}

uint32_t vgic_num_lr(void) { return vgic_nr_lr; }

/* --- GICD/GICR MMIO emulation (windows left stage-2-unmapped) --- */

#define GICD_IPA_BASE 0x08000000ULL
#define GICD_IPA_END  0x08010000ULL
#define GICR_IPA_BASE 0x080A0000ULL
#define GICR_IPA_END  0x080C0000ULL

#define R_GICD_CTLR           0x0000
#define R_GICD_ISENABLER0     0x0100
#define R_GICR_WAKER          0x0014
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
  uint64_t off = (ipa >= GICR_IPA_BASE) ? (ipa - GICR_IPA_BASE)
                                        : (ipa - GICD_IPA_BASE);
  uint32_t size_mask =
      (size_bytes >= 4) ? 0xFFFFFFFFU : ((1U << (size_bytes * 8)) - 1U);

  if (is_write) {
    uint32_t w = (uint32_t)*val & size_mask;
    switch (off) {
    case R_GICD_CTLR:
      vd.gicd_ctlr = w & (GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1NS);
      break;
    case R_GICD_ISENABLER0:     vd.gicd_isenabler0 |= w; break;
    case R_GICR_WAKER:          /* ProcessorSleep handled on read */ break;
    case R_GICR_SGI_IGROUPR0:   vd.gicr_igroupr0 = w; break;
    case R_GICR_SGI_IGRPMODR0:  vd.gicr_igrpmodr0 = w; break;
    case R_GICR_SGI_ISENABLER0: vd.gicr_isenabler0 |= w; break;
    default: break;
    }
    return;
  }

  uint32_t r = 0;
  switch (off) {
  case R_GICD_CTLR:           r = vd.gicd_ctlr; break;
  case R_GICD_ISENABLER0:     r = vd.gicd_isenabler0; break;
  case R_GICR_WAKER:          r = 0; /* ChildrenAsleep clear -> poll exits */ break;
  case R_GICR_SGI_IGROUPR0:   r = vd.gicr_igroupr0; break;
  case R_GICR_SGI_IGRPMODR0:  r = vd.gicr_igrpmodr0; break;
  case R_GICR_SGI_ISENABLER0: r = vd.gicr_isenabler0; break;
  default: r = 0; break;
  }
  *val = r & size_mask;
}

static uint64_t lr_pending(uint32_t intid) {
  return ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
         (0xA0ULL << ICH_LR_PRIO_SHIFT) | (uint64_t)intid;
}

void vgic_inject_ppi(uint32_t intid) {
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    uint64_t lr = lr_read(i);
    if ((lr & ICH_LR_STATE_MASK) == 0) {
      lr_write(i, lr_pending(intid));
      return;
    }
    if ((uint32_t)(lr & 0xFFFFFFFFULL) == intid) {
      return; /* already pending for this INTID */
    }
  }
  /* No free LR - guest behind on a periodic IRQ; drop. */
}
