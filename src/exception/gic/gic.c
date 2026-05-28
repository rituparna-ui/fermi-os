#include "gic.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "strings/strings.h"

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

/* Per-INTID counters. Sized to cover the SGI/PPI/SPI range we actually
 * use (timer PPI 30, GIC SPIs from 32 onward). 256 = 1 KiB of state and
 * comfortably covers the QEMU virt machine. INTIDs >= 256 are silently
 * ignored — no kernel correctness impact, just no /proc visibility. */
#define GIC_COUNTERS_MAX 256
static uint64_t irq_counts[GIC_COUNTERS_MAX];

void gic_count_irq(uint32_t intid) {
  if (intid < GIC_COUNTERS_MAX) {
    irq_counts[intid]++;
  }
}

static const char *gic_intid_source(uint32_t intid) {
  if (intid == 30)  return "timer (PPI)";
  if (intid < 16)   return "SGI";
  if (intid < 32)   return "PPI";
  return "SPI";
}

int gic_render_interrupts(char *buf, uint32_t buflen) {
  uint32_t pos = 0;
  static const char header[] = "INTID  COUNT     SOURCE\n";
  for (uint32_t i = 0; i < sizeof(header) - 1 && pos < buflen; i++) {
    buf[pos++] = header[i];
  }
  for (uint32_t i = 0; i < GIC_COUNTERS_MAX; i++) {
    if (irq_counts[i] == 0)         continue;
    if (pos >= buflen)              break;
    char tmp[64];
    int n = ksnprintf(tmp, sizeof(tmp), "%u    %u  %s\n",
                      (uint64_t)i, irq_counts[i], gic_intid_source(i));
    if (n <= 0) continue;
    uint32_t to_copy = (uint32_t)n;
    if (to_copy > buflen - pos) to_copy = buflen - pos;
    memcpy(buf + pos, tmp, to_copy);
    pos += to_copy;
  }
  return (int)pos;
}


void gic_end_irq(uint64_t intid) {
  __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"(intid));
}
