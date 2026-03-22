#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/*
 * QEMU virt machine PCI layout:
 *   ECAM base:       0x4010000000 (256 buses)
 *   PIO window:      0x3eff0000 (64K)
 *   32-bit MMIO:     0x10000000 - 0x3efeffff
 *   64-bit MMIO:     0x8000000000 - 0xffffffffff
 */
#define PCI_ECAM_BASE 0x4010000000UL
#define PCI_MMIO32_BASE 0x10000000UL
#define PCI_MMIO32_LIMIT 0x3EFEFFFFUL
#define PCI_MMIO64_BASE 0x8000000000UL
#define PCI_MMIO64_LIMIT 0xFFFFFFFFFFUL

#define MAX_PCI_BUS 256
#define MAX_PCI_SLOT 32
#define MAX_PCI_FUNC 8

/* PCI Config Space Offsets */
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02

void pci_enumerate_bus(void);

uint32_t pci_config_read32(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
uint16_t pci_config_read16(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
uint8_t pci_config_read8(uint16_t bus, uint8_t slot, uint8_t func,
                         uint16_t offset);

void pci_config_write32(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t val);
void pci_config_write16(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t val);
void pci_config_write8(uint16_t bus, uint8_t slot, uint8_t func,
                       uint16_t offset, uint8_t val);

#endif