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

  /* Enable PPI 26 (EL2 physical-timer / CNTHP — per-guest vtimer) and PPI 28
   * (EL2 virtual-timer / CNTHV — the hypervisor's own scheduler tick) in the
   * redistributor. */
  gicr_w32(HYP_GICR_SGI_BASE, GICR_SGI_ISENABLER0, (1U << 26) | (1U << 28));

  /* EL2 host CPU interface: accept all priorities, enable Group1. */
  __asm__ __volatile__("msr icc_pmr_el1, %0" ::"r"(0xFFULL));
  __asm__ __volatile__("msr icc_igrpen1_el1, %0\n\tisb" ::"r"(0x1ULL));

  hyp_puts("[HGIC] EL2 host GIC up, PPI 26 (CNTHP) enabled\n");
}

uint32_t hyp_gic_ack(void) {
  uint64_t iar;
  __asm__ __volatile__("mrs %0, icc_iar1_el1" : "=r"(iar));
  return (uint32_t)iar;
}

void hyp_gic_eoi(uint32_t intid) {
  __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"((uint64_t)intid));
}
