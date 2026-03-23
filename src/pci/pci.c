#include "pci.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

static struct pci_device pci_devices[MAX_PCI_DEVICES];
static uint16_t pci_device_count = 0;

static uintptr_t mmio32_next = PCI_MMIO32_BASE;
static uintptr_t mmio64_next = PCI_MMIO64_BASE;

static uintptr_t alloc_mmio32(uint32_t size) {
  /* Align up to the BAR's natural alignment */
  uintptr_t mask = (uintptr_t)size - 1;
  mmio32_next = (mmio32_next + mask) & ~mask;

  if (mmio32_next + size > PCI_MMIO32_LIMIT) {
    uart_errorln("[PCI] 32-bit MMIO space exhausted");
    return 0;
  }

  uintptr_t addr = mmio32_next;
  mmio32_next += size;
  return addr;
}

static uintptr_t alloc_mmio64(uint64_t size) {
  /* Align up to the BAR's natural alignment */
  uintptr_t mask = (uintptr_t)size - 1;
  mmio64_next = (mmio64_next + mask) & ~mask;

  if (mmio64_next + size > PCI_MMIO64_LIMIT) {
    uart_errorln("[PCI] 64-bit MMIO space exhausted");
    return 0;
  }

  uintptr_t addr = mmio64_next;
  mmio64_next += size;
  return addr;
}

static uintptr_t pci_make_ecam_addr(uint16_t bus, uint8_t slot, uint8_t func,
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
  uint32_t size = ~size_mask + 1;

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
    /* Amount of memory the device needs for its registers to be memory mapped
     */
    uint32_t size = pci_get_bar_size(b, d, f, bar_offset);

    if (size == 0 || size == 0xFFFFFFFF) {
      // unused bar
      dev->bar_addr[i] = 0;
      continue;
    }

    if (type == 0x00) {
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
      uart_println("[PCI][64 Bit Memory Space]");
      uart_puts(" BAR");
      uart_putdec(i);
      uart_puts(" has size: ");
      uart_puthex(size);
      uart_println("");
      uintptr_t addr = alloc_mmio64(size);
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

static const char *virtio_cfg_type_name(uint8_t type) {
  switch (type) {
  case 1:
    return "COMMON_CFG";
  case 2:
    return "NOTIFY_CFG";
  case 3:
    return "ISR_CFG";
  case 4:
    return "DEVICE_CFG";
  case 5:
    return "PCI_CFG";
  default:
    return "UNKNOWN";
  }
}

static void virtio_populate_capabilities(struct pci_device *pci_dev,
                                  struct virtio_pci_caps *pci_caps,
                                  uint8_t cap_ptr) {
  uint8_t b = pci_dev->bus;
  uint8_t d = pci_dev->slot;
  uint8_t f = pci_dev->func;

  /* virtio_pci_cap layout in config space (virtio spec §4.1.4):
   *   cap_ptr + 0x00: cap_vndr  (u8)
   *   cap_ptr + 0x01: cap_next  (u8)
   *   cap_ptr + 0x02: cap_len   (u8)
   *   cap_ptr + 0x03: cfg_type  (u8)
   *   cap_ptr + 0x04: bar       (u8)
   *   cap_ptr + 0x05: padding   (u8 x3)
   *   cap_ptr + 0x08: offset    (u32)
   *   cap_ptr + 0x0C: length    (u32)
   */
  uint8_t cfg_type = pci_config_read8(b, d, f, cap_ptr + 3);
  uint8_t bar = pci_config_read8(b, d, f, cap_ptr + 4);
  uint32_t offset = pci_config_read32(b, d, f, cap_ptr + 8);
  uint32_t length = pci_config_read32(b, d, f, cap_ptr + 0x0C);

  uintptr_t cap_addr = (uintptr_t)(pci_dev->bar_addr[bar] + offset);

  uart_puts("  type=");
  uart_putdec(cfg_type);
  uart_puts(" (");
  uart_puts(virtio_cfg_type_name(cfg_type));
  uart_puts(") bar=");
  uart_putdec(bar);
  uart_puts(" offset=");
  uart_puthex(offset);
  uart_puts(" length=");
  uart_puthex(length);
  uart_puts(" -> addr=");
  uart_puthex(cap_addr);
  uart_println("");

  switch (cfg_type) {
  case 1:
    pci_caps->common_cfg = cap_addr;
    break;
  case 2:
    pci_caps->notify_base = cap_addr;
    /* For notify cap, there is an extra u32 at cap_ptr + 0x10
     * (virtio spec §4.1.4.4) */
    pci_caps->notify_off_multiplier =
        pci_config_read32(b, d, f, cap_ptr + 0x10);
    uart_puts("  notify_off_multiplier=");
    uart_puthex(pci_caps->notify_off_multiplier);
    uart_println("");
    break;
  case 3:
    pci_caps->isr_cfg = cap_addr;
    break;
  case 4:
    pci_caps->device_cfg = cap_addr;
    break;
    /* type 5 = PCI_CFG - alternative config space access, not needed with ECAM
     */
  }
}

void pci_parse_capabilities(struct pci_device *dev,
                            struct virtio_pci_caps *caps) {
  uint8_t bus = dev->bus;
  uint8_t slot = dev->slot;
  uint8_t func = dev->func;

  uint16_t status = pci_config_read16(bus, slot, func, PCI_STATUS);

  if (!(status & (1 << 4))) {
    uart_errorln("[PCI] Capabilities not present");
    return;
  }

  uint8_t cap_ptr = pci_config_read8(bus, slot, func, PCI_CAP_PTR);

  while (cap_ptr) {
    uint8_t cap_id = pci_config_read8(bus, slot, func, cap_ptr);
    uint8_t next = pci_config_read8(bus, slot, func, cap_ptr + 1);

    uart_puts("[PCI] CapId: ");
    uart_puthex(cap_id);

    if (cap_id == 0x11) {
      uart_println(" MSI-X 0x11. Ignoring for now");
    } else if (cap_id == 0x09) {
      uart_println(" Vendor specific 0x09");
      virtio_populate_capabilities(dev, caps, cap_ptr);
    } else {
      uart_println(" Other Cap Type");
    }

    cap_ptr = next;
  }
}