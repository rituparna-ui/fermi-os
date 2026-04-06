#include "gic.h"
#include "mmio/mmio.h"
#include "uart/uart.h"

static void enable_system_register_interface() {
  uart_println("[GIC] Enabling System Register Interface");

  uint64_t sre;
  __asm__ __volatile__("mrs %0, icc_sre_el1" : "=r"(sre));
  sre |= 1;
  __asm__ __volatile__("msr icc_sre_el1, %0" ::"r"(sre));
  __asm__ __volatile__("isb");
}

static void enable_distributor_affinity_routing() {
  uart_println("[GIC] Enabling Distributor affinity routing");

  mmio_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);

  uart_printf("[GIC] GICD_CTLR = %x\n", (uint64_t)mmio_read32(GICD_CTLR));
}

static void redistributor_wakeup() {
  uint32_t waker = mmio_read32(GICR_WAKER);
  waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
  mmio_write32(GICR_WAKER, waker);

  // Poll until ChildrenAsleep clears
  while (mmio_read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
  }
  uart_println("[GIC] Redistributor awake");
}

void gic_init() {
  uart_println("[GIC] Initializing GICv3");

  enable_system_register_interface();
  enable_distributor_affinity_routing();
  redistributor_wakeup();

  // Mark all SGIs/PPIs (0-31) as G1NS
  // IGROUPR0 = all 1s (Group 1), IGRPMODR0 = all 0s (Non-secure)
  mmio_write32(GICR_IGROUPR0, 0xFFFFFFFF);
  mmio_write32(GICR_IGRPMODR0, 0x00000000);

  // priority mask, accept all priorities
  __asm__ __volatile__("msr icc_pmr_el1, %0" ::"r"(0xFFULL));

  // Enable Group 1 interrupts at CPU interface
  __asm__ __volatile__("msr icc_igrpen1_el1, %0" ::"r"(0x01ULL));
  __asm__ __volatile__("isb");

  // Unmask IRQs
  __asm__ __volatile__("msr daifclr, #2");

  uart_println("[GIC] Initialized! IRQs enabled");

  return;
}

void gic_enable_irq(uint32_t intid) {
  if (intid < 32) {
    // SGI/PPI: Redistributor ISENABLER0
    uint32_t val = mmio_read32(GICR_ISENABLER0);
    val |= (1U << intid);
    mmio_write32(GICR_ISENABLER0, val);
  } else {
    // SPI: Distributor ISENABLER[n]
    uint32_t reg = GICD_ISENABLER + (intid / 32) * 4;
    uint32_t bit = intid % 32;
    uint32_t val = mmio_read32(reg);
    val |= (1U << bit);
    mmio_write32(reg, val);
  }

  uart_printf("[GIC] Enabled IRQ %d\n", (uint64_t)intid);
}

uint64_t gic_ack_irq() {
  uint64_t ack;
  __asm__ __volatile__("mrs %0, icc_iar1_el1" : "=r"(ack));
  return ack;
}

void gic_end_irq(uint64_t intid) {
  __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"(intid));
}