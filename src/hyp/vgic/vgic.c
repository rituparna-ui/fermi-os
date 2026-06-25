#include "vgic.h"
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

  /* Clear all implemented List Registers + active-priority regs. */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    lr_write(i, 0);
  }
  __asm__ __volatile__("msr ich_ap0r0_el2, %0" ::"r"(0ULL));
  __asm__ __volatile__("msr ich_ap1r0_el2, %0" ::"r"(0ULL));

  /* Seed the virtual control regs and enable the virtual CPU interface. */
  __asm__ __volatile__("msr ich_vmcr_el2, %0" ::"r"(ICH_VMCR_SEED));
  __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(ICH_HCR_EN));

  hyp_puts("[VGIC] ICC_SRE_EL2=");
  hyp_puthex(sre);
  hyp_puts(" VPL=");
  hyp_puthex(vgic_nr_lr);
  hyp_puts(" (virtual CPU interface enabled)\n");
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
#define R_GICR_WAKER       0x0014
#define R_GICR_SGI_IGROUPR0   0x10080
#define R_GICR_SGI_IGRPMODR0  0x10D00
#define R_GICR_SGI_ISENABLER0 0x10100

#define GICD_CTLR_ARE_NS  (1U << 4)
#define GICD_CTLR_EN_G1NS (1U << 1)

static struct {
  uint32_t gicd_ctlr;
  uint32_t gicd_isenabler0; /* SPIs 0..31 (guest only enables PPIs though) */
  uint32_t gicr_igroupr0;
  uint32_t gicr_igrpmodr0;
  uint32_t gicr_isenabler0; /* SGIs/PPIs 0..31 — guest enables INTID 30 here */
} vd;

int vgic_mmio_is_target(uint64_t ipa) {
  return (ipa >= GICD_IPA_BASE && ipa < GICD_IPA_END) ||
         (ipa >= GICR_IPA_BASE && ipa < GICR_IPA_END);
}

void vgic_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                       int size_bytes) {
  (void)size_bytes;
  uint64_t off;
  if (ipa >= GICR_IPA_BASE) {
    off = ipa - GICR_IPA_BASE;
  } else {
    off = ipa - GICD_IPA_BASE;
  }

  if (is_write) {
    uint32_t w = (uint32_t)*val;
    switch (off) {
    case R_GICD_CTLR:        vd.gicd_ctlr = w & (GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1NS); break;
    case R_GICD_ISENABLER0:  vd.gicd_isenabler0 |= w; break;
    case R_GICR_WAKER:       /* ProcessorSleep handled on read; ignore write */ break;
    case R_GICR_SGI_IGROUPR0:   vd.gicr_igroupr0 = w; break;
    case R_GICR_SGI_IGRPMODR0:  vd.gicr_igrpmodr0 = w; break;
    case R_GICR_SGI_ISENABLER0: vd.gicr_isenabler0 |= w; break;
    default: /* unmodelled write — drop silently */ break;
    }
    return;
  }

  /* Read. */
  uint32_t r = 0;
  switch (off) {
  case R_GICD_CTLR:        r = vd.gicd_ctlr; break;
  case R_GICD_ISENABLER0:  r = vd.gicd_isenabler0; break;
  case R_GICR_WAKER:       r = 0; /* ProcessorSleep=0, ChildrenAsleep=0 — the
                                   * guest's poll loop (gic.c:30) exits */ break;
  case R_GICR_SGI_IGROUPR0:   r = vd.gicr_igroupr0; break;
  case R_GICR_SGI_IGRPMODR0:  r = vd.gicr_igrpmodr0; break;
  case R_GICR_SGI_ISENABLER0: r = vd.gicr_isenabler0; break;
  default: r = 0; break;
  }
  *val = r;
}

void vgic_inject_ppi(uint32_t intid) {
  /* Find a free LR (State == Invalid) that does not already hold this INTID. */
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    uint64_t lr = lr_read(i);
    if ((lr & ICH_LR_STATE_MASK) == 0) {
      uint64_t v = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
                   (0xA0ULL << ICH_LR_PRIO_SHIFT) | (uint64_t)intid;
      lr_write(i, v);
      return;
    }
    /* Already pending/active for this INTID — guest hasn't consumed it yet. */
    if ((uint32_t)(lr & 0xFFFFFFFFULL) == intid) {
      return;
    }
  }
  /* No free LR: guest is behind on this periodic IRQ; drop silently. */
}
