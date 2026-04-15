#include "virtio.h"
#include "uart/uart.h"

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

void virtio_populate_capabilities(struct pci_device *pci_dev,
                                  struct virtio_pci_caps *pci_caps,
                                  uint8_t cap_ptr) {
  uint8_t b = pci_dev->bus;
  uint8_t d = pci_dev->slot;
  uint8_t f = pci_dev->func;

  /* virtio_pci_cap layout in config space (virtio 4.1.4):
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

  uart_printf("  type=%d (%s) bar=%d offset=%x length=%x -> addr=%x\n",
              cfg_type, virtio_cfg_type_name(cfg_type), bar, offset, length,
              cap_addr);

  switch (cfg_type) {
  case 1:
    pci_caps->common_cfg = cap_addr;
    break;
  case 2:
    pci_caps->notify_base = cap_addr;
    /* For notify cap, there is an extra u32 at cap_ptr + 0x10
     * (virtio spec 4.1.4.4) */
    pci_caps->notify_off_multiplier =
        pci_config_read32(b, d, f, cap_ptr + 0x10);
    uart_printf("  notify_off_multiplier=%x\n",
                pci_caps->notify_off_multiplier);
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

void virtio_parse_capabilities(struct pci_device *dev,
                               struct virtio_pci_caps *caps) {
  uint8_t bus = dev->bus;
  uint8_t slot = dev->slot;
  uint8_t func = dev->func;

  uint16_t status = pci_config_read16(bus, slot, func, PCI_STATUS);

  if (!(status & (1 << 4))) {
    uart_errorln("[PCI] Capabilities not present");
    return;
  }

  int cap_ptr = pci_config_read8(bus, slot, func, PCI_CAP_PTR);

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