#include "pci.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

struct pci_device pci_devices[MAX_PCI_DEVICES];
uint16_t pci_device_count = 0;

uintptr_t pci_make_ecam_addr(uint16_t bus, uint8_t slot, uint8_t func,
                             uint16_t offset) {
  return PCI_ECAM_BASE | ((uintptr_t)bus << 20) | ((uintptr_t)slot << 15) |
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
  return mmio_write32(pci_make_ecam_addr(bus, slot, func, offset), val);
}

void pci_config_write16(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t val) {
  return mmio_write16(pci_make_ecam_addr(bus, slot, func, offset), val);
}
void pci_config_write8(uint16_t bus, uint8_t slot, uint8_t func,
                       uint16_t offset, uint8_t val) {
  return mmio_write8(pci_make_ecam_addr(bus, slot, func, offset), val);
}

void pci_log_device_found(uint16_t bus, uint8_t slot, uint8_t func,
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

        if (pci_device_count >= MAX_PCI_DEVICES) {
          uart_errorln("[PCI] Max PCI devices limit reached");
          return;
        }

        pci_devices[pci_device_count] = (struct pci_device){
            .bus = bus,
            .slot = slot,
            .func = func,
            .vendor_id = vendor_id,
            .device_id = device_id,
        };
        pci_device_count++;
      }
    }
  }
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *pci_device) {
  for (uint64_t i = 0; i < pci_device_count; i++) {
    struct pci_device current_dev = pci_devices[i];

    if (current_dev.vendor_id == vendor_id &&
        current_dev.device_id == device_id) {
      *pci_device = current_dev;
      return ESUCCESS;
    }
  }
  return EERROR;
}