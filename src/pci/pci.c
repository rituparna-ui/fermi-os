#include "pci.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include <stdint.h>

// ECAM addresses passed to mmio_read/write are physical.
// The MMIO layer adds KERNEL_VA_OFFSET (after mmio_switch_to_upper)
// to access them through TTBR1 upper half.

static uintptr_t pci_make_ecam_addr(uint16_t bus, uint8_t slot, uint8_t func,
                                    uint16_t offset) {
  return PCI_ECAM_PHYS | ((uintptr_t)bus << 20) | ((uintptr_t)slot << 15) |
         ((uintptr_t)func << 12) | (uintptr_t)offset;
}

uint32_t pci_config_read32(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  return mmio_read32(pci_make_ecam_addr(bus, slot, func, offset));
}

uint16_t pci_config_read16(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  return mmio_read16(pci_make_ecam_addr(bus, slot, func, offset));
}

uint8_t pci_config_read8(uint16_t bus, uint8_t slot, uint8_t func,
                         uint16_t offset) {
  return mmio_read8(pci_make_ecam_addr(bus, slot, func, offset));
}

void pci_config_write32(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t val) {
  mmio_write32(pci_make_ecam_addr(bus, slot, func, offset), val);
}

void pci_config_write16(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t val) {
  mmio_write16(pci_make_ecam_addr(bus, slot, func, offset), val);
}

void pci_config_write8(uint16_t bus, uint8_t slot, uint8_t func,
                       uint16_t offset, uint8_t val) {
  mmio_write8(pci_make_ecam_addr(bus, slot, func, offset), val);
}

static void pci_log_device_found(uint16_t bus, uint8_t slot, uint8_t func,
                                 uint16_t vendor_id, uint16_t device_id) {
  uart_puts("[PCI] Device found at ");
  uart_putdec(bus);
  uart_puts(":");
  uart_putdec(slot);
  uart_puts(".");
  uart_putdec(func);
  uart_puts(" | VendorID: ");
  uart_puthex(vendor_id);
  uart_puts(", DeviceID: ");
  uart_puthex(device_id);
  uart_println("");
}

void pci_enumerate_bus() {
  uart_println("[PCI] Enumerating PCI Devices");

  for (uint16_t bus = 0; bus < MAX_PCI_BUS; bus++) {
    for (uint8_t slot = 0; slot < MAX_PCI_SLOT; slot++) {
      for (uint8_t func = 0; func < MAX_PCI_FUNC; func++) {
        uint16_t vendor_id = pci_config_read16(bus, slot, func, PCI_VENDOR_ID);

        if (vendor_id == 0xFFFF) {
          continue;
        }

        uint16_t device_id = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
        pci_log_device_found(bus, slot, func, vendor_id, device_id);
      }
    }
  }
}
