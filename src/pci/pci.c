#include "pci.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

static struct pci_device pci_devices[MAX_PCI_DEVICES];
static uint16_t pci_device_count = 0;

static uintptr_t mmio32_next = PCI_MMIO32_PHYS;
static uintptr_t mmio64_next = PCI_MMIO64_PHYS;

static uintptr_t alloc_mmio32(uint32_t size) {
  /* Align up to the BAR's natural alignment */
  uintptr_t mask = (uintptr_t)size - 1;
  mmio32_next = (mmio32_next + mask) & ~mask;

  if (mmio32_next & (size - 1)) {
    mmio32_next = (mmio32_next + size) & ~(size - 1);
  }

  uintptr_t addr = mmio32_next;
  mmio32_next += size;
  return addr;
}

static uintptr_t alloc_mmio64(uint64_t size) {
  /* Align up to the BAR's natural alignment */
  uintptr_t mask = (uintptr_t)size - 1;
  mmio64_next = (mmio64_next + mask) & ~mask;

  if (mmio64_next & (size - 1)) {
    mmio64_next = (mmio64_next + size) & ~(size - 1);
  }

  uintptr_t addr = mmio64_next;
  mmio64_next += size;
  return addr;
}

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
  uart_printf("[PCI] Device found at %d:%d.%d | VendorID: %x, DeviceID: %x\n",
              bus, slot, func, vendor_id, device_id);
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
  for (uint16_t i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].vendor_id == vendor_id &&
        pci_devices[i].device_id == device_id) {
      *pci_device = pci_devices[i];
      return ESUCCESS;
    }
  }
  return EERROR;
}

uint8_t pci_get_header_type(struct pci_device *dev) {
  uint8_t header_type =
      pci_config_read8(dev->bus, dev->slot, dev->func, PCI_HEADER_TYPE);
  return header_type;
}

static uint32_t pci_get_bar_size(uint8_t bus, uint8_t slot, uint8_t func,
                                 uint16_t offset) {
  uint32_t original = pci_config_read32(bus, slot, func, offset);
  pci_config_write32(bus, slot, func, offset, 0xFFFFFFFF);
  uint32_t size_mask = pci_config_read32(bus, slot, func, offset);
  pci_config_write32(bus, slot, func, offset, original);

  // mask lower 4 bits (control bits)
  size_mask &= ~0xF;
  if (size_mask == 0) {
    // unimplemented BAR
    return 0;
  }
  uint32_t size = ~size_mask + 1;

  return size;
}

// For 64-bit BARs, probe both BAR[i] and BAR[i+1] to get the full size
static uint64_t pci_get_bar_size64(uint8_t bus, uint8_t slot, uint8_t func,
                                   uint16_t offset_lo, uint16_t offset_hi) {
  // Save originals
  uint32_t orig_lo = pci_config_read32(bus, slot, func, offset_lo);
  uint32_t orig_hi = pci_config_read32(bus, slot, func, offset_hi);

  // Write all-1s to both halves
  pci_config_write32(bus, slot, func, offset_lo, 0xFFFFFFFF);
  pci_config_write32(bus, slot, func, offset_hi, 0xFFFFFFFF);

  // Read back
  uint32_t mask_lo = pci_config_read32(bus, slot, func, offset_lo);
  uint32_t mask_hi = pci_config_read32(bus, slot, func, offset_hi);

  // Restore originals
  pci_config_write32(bus, slot, func, offset_lo, orig_lo);
  pci_config_write32(bus, slot, func, offset_hi, orig_hi);

  // Mask lower 4 control bits of the low word
  uint64_t mask = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xFUL);
  uint64_t size = ~mask + 1;

  return size;
}

void pci_assign_bars(struct pci_device *dev) {
  uart_println("[PCI] Assigning BARs");

  uint8_t b = dev->bus;
  uint8_t d = dev->slot;
  uint8_t f = dev->func;

  for (uint8_t i = 0; i < 6; i++) {
    uint32_t bar_offset = PCI_BAR0 + i * 4;
    uint32_t bar = pci_config_read32(b, d, f, bar_offset);

    /* https://wiki.osdev.org/PCI */
    /* I/O Space BAR Layout */
    if (bar & 0x01) {
      uart_errorln("[PCI] IO BAR Type, Ignoring");
      continue;
    }

    /* Memory Space BAR Layout */
    uint8_t type = (bar >> 1) & 0x03;

    if (type == 0x00) {
      /* 32-bit BAR */
      uint32_t size = pci_get_bar_size(b, d, f, bar_offset);

      if (size == 0 || size == 0xFFFFFFFF) {
        dev->bar_addr[i] = 0;
        continue;
      }

      uart_println("[PCI][32 Bit Memory Space]");
      uart_puts(" BAR");
      uart_putdec(i);
      uart_puts(" has size: ");
      uart_puthex(size);
      uart_println("");
      uintptr_t addr = alloc_mmio32(size);

      pci_config_write32(b, d, f, bar_offset, (uint32_t)addr);
      dev->bar_addr[i] = addr;
    } else if (type == 0x02) {
      /* 64-bit BAR — probe both halves for correct size */
      uint32_t bar_offset_hi = PCI_BAR0 + (i + 1) * 4;
      uint64_t size64 = pci_get_bar_size64(b, d, f, bar_offset, bar_offset_hi);

      if (size64 == 0) {
        dev->bar_addr[i] = 0;
        i++; // skip upper BAR
        continue;
      }

      uart_println("[PCI][64 Bit Memory Space]");
      uart_puts(" BAR");
      uart_putdec(i);
      uart_puts(" has size: ");
      uart_puthex(size64);
      uart_println("");
      uintptr_t addr = alloc_mmio64(size64);
      // lower 32 bit
      pci_config_write32(b, d, f, bar_offset, (uint32_t)(addr & 0xFFFFFFFF));
      // upper 32 bit
      pci_config_write32(b, d, f, bar_offset + 4, (uint32_t)(addr >> 32));
      dev->bar_addr[i] = addr;
      i++;
    } else {
      uart_errorln("[PCI][Memory Space Type] Huh ?");
    }
  }

  uart_println("[PCI] BARs Assigned");
}

void pci_enable_device(struct pci_device *dev) {
  uart_println("[PCI] Enabling device");

  /* https://wiki.osdev.org/PCI#Command_Register */
  uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
  cmd |= (1 << 1); // Memory Space Enable
  cmd |= (1 << 2); // Bus Master Enable (DMA)

  pci_config_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
  uart_println("[PCI] Device Enabled");
}