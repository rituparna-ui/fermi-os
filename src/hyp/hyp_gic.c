#include "hyp_gic.h"
#include "hyp.h"
#include <stdint.h>

/* GICD/GICR MMIO accessors (EL2 MMU off => physical == VA). */
static inline void gicd_w32(uint64_t off, uint32_t v) {
  *(volatile uint32_t *)(HYP_GICD_BASE + off) = v;
}
static inline uint32_t gicr_r32(uint64_t base, uint64_t off) {
  return *(volatile uint32_t *)(base + off);
}
static inline void gicr_w32(uint64_t base, uint64_t off, uint32_t v) {
  *(volatile uint32_t *)(base + off) = v;
}

#define GICD_CTLR        0x0000
#define GICD_CTLR_ARE_NS (1U << 4)
#define GICD_CTLR_EN_G1NS (1U << 1)

#define GICR_WAKER         0x0014
#define GICR_WAKER_PS      (1U << 1) /* ProcessorSleep   */
#define GICR_WAKER_CA      (1U << 2) /* ChildrenAsleep   */
#define GICR_SGI_IGROUPR0  0x0080
#define GICR_SGI_ISENABLER0 0x0100

void hyp_gic_init(void) {
  /* Distributor: affinity routing + Group1-NS enable (the guest will also
   * touch GICD via straight-through MMIO; both EL2 and guest program the same
   * real distributor in this phase — fine, the writes are idempotent). */
  gicd_w32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1NS);

  /* Wake our redistributor: clear ProcessorSleep, wait for ChildrenAsleep. */
  uint32_t waker = gicr_r32(HYP_GICR_BASE, GICR_WAKER);
  waker &= ~GICR_WAKER_PS;
  gicr_w32(HYP_GICR_BASE, GICR_WAKER, waker);
  while (gicr_r32(HYP_GICR_BASE, GICR_WAKER) & GICR_WAKER_CA) {
  }

  /* Mark all SGIs/PPIs Group1-NS. */
  gicr_w32(HYP_GICR_SGI_BASE, GICR_SGI_IGROUPR0, 0xFFFFFFFFU);

  /* Enable PPI 26 (EL2 physical-timer / CNTHP), PPI 28 (EL2 virtual-timer /
   * CNTHV), and SGI 0 (the inter-core reschedule IPI, so pCPU0 can be poked by a
   * secondary) in the redistributor. */
  gicr_w32(HYP_GICR_SGI_BASE, GICR_SGI_ISENABLER0, (1U << 26) | (1U << 28) | (1U << 0));

  /* EL2 host CPU interface: accept all priorities, enable Group1. */
  __asm__ __volatile__("msr icc_pmr_el1, %0" ::"r"(0xFFULL));
  __asm__ __volatile__("msr icc_igrpen1_el1, %0\n\tisb" ::"r"(0x1ULL));

  hyp_puts("[HGIC] EL2 host GIC up, PPI 26 (CNTHP) enabled\n");
}

/* GICR per-redistributor stride (RD frame + SGI frame = 2 x 64 KiB). */
#define GICR_STRIDE      0x20000ULL
#define GICR_TYPER       0x0008      /* 64-bit; Affinity in bits[63:32] */
#define GICR_TYPER_LAST  (1ULL << 4) /* last redistributor in the region */

/* This core's MPIDR affinity packed as GICR_TYPER.Affinity expects. */
static uint32_t my_gicr_affinity(void) {
  uint64_t m;
  __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(m));
  uint32_t aff0 = m & 0xFF, aff1 = (m >> 8) & 0xFF, aff2 = (m >> 16) & 0xFF;
  uint32_t aff3 = (m >> 32) & 0xFF;
  return aff0 | (aff1 << 8) | (aff2 << 16) | (aff3 << 24);
}

uint64_t hyp_gic_percpu_init(int enable_cnthp) {
  /* Discover this core's redistributor by matching GICR_TYPER affinity to MPIDR
   * (frames are not guaranteed in MPIDR order, so don't assume cpu*stride). */
  uint32_t want = my_gicr_affinity();
  uint64_t rd = HYP_GICR_BASE, found = 0;
  for (int i = 0; i < 16; i++) {
    uint64_t typer = *(volatile uint64_t *)(rd + GICR_TYPER);
    if ((uint32_t)(typer >> 32) == want) {
      found = rd;
      break;
    }
    if (typer & GICR_TYPER_LAST) {
      break;
    }
    rd += GICR_STRIDE;
  }
  if (!found) {
    hyp_panic("hyp_gic_percpu_init: no redistributor matches this core's MPIDR");
  }
  uint64_t sgi = found + 0x10000ULL;

  /* Wake this redistributor (bounded spin so a wedged GICR diagnoses). */
  uint32_t waker = gicr_r32(found, GICR_WAKER) & ~GICR_WAKER_PS;
  gicr_w32(found, GICR_WAKER, waker);
  int spins = 1000000;
  while ((gicr_r32(found, GICR_WAKER) & GICR_WAKER_CA) && --spins) {
  }
  if (spins == 0) {
    hyp_panic("hyp_gic_percpu_init: GICR_WAKER.ChildrenAsleep stuck");
  }

  gicr_w32(sgi, GICR_SGI_IGROUPR0, 0xFFFFFFFFU); /* SGIs/PPIs Group1-NS */
  if (enable_cnthp) {
    /* Scheduling core: enable PPI 26 (CNTHP slice/vtimer) + SGI 0 (the
     * inter-core reschedule IPI hyp_send_resched_sgi targets). */
    gicr_w32(sgi, GICR_SGI_ISENABLER0, (1U << 26) | (1U << 0));
  }

  /* EL2 host CPU interface: accept all priorities, enable Group1. */
  __asm__ __volatile__("msr icc_pmr_el1, %0" ::"r"(0xFFULL));
  __asm__ __volatile__("msr icc_igrpen1_el1, %0\n\tisb" ::"r"(0x1ULL));
  return found;
}

uint32_t hyp_gic_ack(void) {
  uint64_t iar;
  __asm__ __volatile__("mrs %0, icc_iar1_el1" : "=r"(iar));
  return (uint32_t)iar;
}

void hyp_gic_eoi(uint32_t intid) {
  __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"((uint64_t)intid));
}
