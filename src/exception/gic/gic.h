#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#define GICD_BASE 0x08000000UL
#define GICR_BASE 0x080A0000UL

#define GICD_CTLR (GICD_BASE + 0x0000)
#define GICD_ISENABLER (GICD_BASE + 0x0100)

#define GICD_CTLR_ENABLE_G1NS (1U << 1)
#define GICD_CTLR_ARE_NS (1U << 4)

#define GICR_WAKER (GICR_BASE + 0x0014)
#define GICR_SGI_BASE (GICR_BASE + 0x10000)
#define GICR_IGROUPR0 (GICR_SGI_BASE + 0x0080)
#define GICR_IGRPMODR0 (GICR_SGI_BASE + 0x0D00)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x0100)
#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

#define GIC_INTID_NO_PENDING 1023

void gic_init(void);
void gic_enable_irq(uint32_t intid);
uint64_t gic_ack_irq(void);
void gic_end_irq(uint64_t intid);

#endif