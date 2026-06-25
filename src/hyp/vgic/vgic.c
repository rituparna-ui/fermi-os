#include "vgic.h"
#include "../vcpu.h" /* vcpu_vgic_t */
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

/* The per-vCPU GICD/GICR software model the MMIO emulator currently acts on.
 * Set by vgic_set_current() at each world entry. For a single guest this is
 * just the one vcpu's model; for multi-guest each vcpu has its own. */
static vcpu_vgic_t *cur;

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
  vgic_nr_lr = (uint32_t)((vtr & 0x1F) + 1); /* ICH_VTR_EL2.ListRegs is 1..32 */
  /* The per-vCPU LR shadow (vcpu_vgic_t.lr[]) is fixed at 16 entries; clamp so
   * vgic_save/restore/inject never iterate past it on hardware with >16 LRs. */
  if (vgic_nr_lr > 16) {
    vgic_nr_lr = 16;
  }

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
  /* Fail-safe: never dereference a NULL per-vCPU model. The caller is supposed
   * to vgic_set_current() before servicing a guest's GICD/GICR MMIO trap; if it
   * somehow did not, a guest access to the (unmapped, trapping) vGIC window
   * would otherwise NULL-deref at EL2 and crash the host. Treat it as "no
   * device": reads return 0, writes are dropped. */
  if (!cur) {
    if (!is_write && val) {
      *val = 0;
    }
    return;
  }
  uint64_t off = (ipa >= GICR_IPA_BASE) ? (ipa - GICR_IPA_BASE)
                                        : (ipa - GICD_IPA_BASE);
  uint32_t size_mask =
      (size_bytes >= 4) ? 0xFFFFFFFFU : ((1U << (size_bytes * 8)) - 1U);

  if (is_write) {
    uint32_t w = (uint32_t)*val & size_mask;
    switch (off) {
    case R_GICD_CTLR:
      cur->gicd_ctlr = w & (GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1NS);
      break;
    case R_GICD_ISENABLER0:     cur->gicd_isenabler0 |= w; break;
    case R_GICR_WAKER:          /* ProcessorSleep handled on read */ break;
    case R_GICR_SGI_IGROUPR0:   cur->gicr_igroupr0 = w; break;
    case R_GICR_SGI_IGRPMODR0:  cur->gicr_igrpmodr0 = w; break;
    case R_GICR_SGI_ISENABLER0: cur->gicr_isenabler0 |= w; break;
    default: break;
    }
    return;
  }

  uint32_t r = 0;
  switch (off) {
  case R_GICD_CTLR:           r = cur->gicd_ctlr; break;
  case R_GICD_ISENABLER0:     r = cur->gicd_isenabler0; break;
  case R_GICR_WAKER:          r = 0; /* ChildrenAsleep clear -> poll exits */ break;
  case R_GICR_SGI_IGROUPR0:   r = cur->gicr_igroupr0; break;
  case R_GICR_SGI_IGRPMODR0:  r = cur->gicr_igrpmodr0; break;
  case R_GICR_SGI_ISENABLER0: r = cur->gicr_isenabler0; break;
  default: r = 0; break;
  }
  *val = r & size_mask;
}

/* --- Per-vCPU vGIC state (multi-guest) --- */

void vgic_set_current(vcpu_vgic_t *g) { cur = g; }

int vgic_intid_enabled(uint32_t intid) {
  if (!cur || intid >= 32) {
    return 0;
  }
  return (cur->gicr_isenabler0 >> intid) & 1;
}

void vgic_vcpu_reset(vcpu_vgic_t *g) {
  g->hcr = ICH_HCR_EN;
  g->vmcr = ICH_VMCR_SEED;
  g->ap0r0 = 0;
  g->ap1r0 = 0;
  for (int i = 0; i < 16; i++) {
    g->lr[i] = 0;
  }
  g->gicd_ctlr = 0;
  g->gicd_isenabler0 = 0;
  g->gicr_igroupr0 = 0;
  g->gicr_igrpmodr0 = 0;
  g->gicr_isenabler0 = 0;
}

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

void vgic_restore(const vcpu_vgic_t *g) {
  __asm__ __volatile__("msr ich_vmcr_el2, %0" ::"r"(g->vmcr));
  __asm__ __volatile__("msr ich_ap0r0_el2, %0" ::"r"(g->ap0r0));
  __asm__ __volatile__("msr ich_ap1r0_el2, %0" ::"r"(g->ap1r0));
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    lr_write(i, g->lr[i]);
  }
  __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(g->hcr));
}

#define ICH_LR_HW (1ULL << 61)

static uint64_t lr_pending(uint32_t intid) {
  return ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
         (0xA0ULL << ICH_LR_PRIO_SHIFT) | (uint64_t)intid;
}

/* HW-mapped pending LR: vINTID and pINTID both = intid. When the guest EOIs the
 * virtual interrupt, the GIC deactivates the PHYSICAL interrupt automatically —
 * so a passed-through level-triggered timer does not need (and must not get) a
 * physical EOI/deactivate from EL2. pINTID is field [44:32]. */
static uint64_t lr_pending_hw(uint32_t intid) {
  return ICH_LR_STATE_PENDING | ICH_LR_HW | ICH_LR_GROUP1 |
         (0x00ULL << ICH_LR_PRIO_SHIFT) |
         ((uint64_t)intid << 32) | (uint64_t)intid;
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

/* Inject a HARDWARE-mapped pending interrupt (for a passed-through physical
 * IRQ such as the guest's EL1 timer). Returns 1 if injected (caller must NOT
 * physically EOI the source), 0 if already in flight / no free LR. */
int vgic_inject_hw(uint32_t intid) {
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    uint64_t lr = lr_read(i);
    if ((lr & ICH_LR_STATE_MASK) != 0 &&
        (uint32_t)(lr & 0xFFFFFFFFULL) == intid) {
      return 0; /* already pending/active for this INTID */
    }
  }
  for (uint32_t i = 0; i < vgic_nr_lr; i++) {
    if ((lr_read(i) & ICH_LR_STATE_MASK) == 0) {
      lr_write(i, lr_pending_hw(intid));
      return 1;
    }
  }
  return 0; /* no free LR */
}

/* Inject a pending Group1 interrupt into a NON-current vCPU's SAVED vGIC state
 * (its lr[] shadow), so it is presented when that vCPU is next restored. Used to
 * signal a peer VM (inter-VM doorbell) while another VM is running. */
void vgic_inject_to(vcpu_vgic_t *g, uint32_t intid) {
  for (int i = 0; i < 16; i++) {
    if ((g->lr[i] & ICH_LR_STATE_MASK) != 0 &&
        (uint32_t)(g->lr[i] & 0xFFFFFFFFULL) == intid) {
      return; /* already pending for this INTID */
    }
  }
  for (int i = 0; i < 16; i++) {
    if ((g->lr[i] & ICH_LR_STATE_MASK) == 0) {
      g->lr[i] = lr_pending(intid);
      return;
    }
  }
}
