#include "rng.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

struct virtio_rng rng_dev;

void pci_virtio_rng_init() {
  uart_println("[RNG] Initializing Device");

  /* Step 0 */
  if (!pci_find_device(VIRTIO_RNG_VENDOR_ID, VIRTIO_RNG_DEVICE_ID,
                       &rng_dev.pci)) {
    uart_errorln("[RNG] Device not found");
    return;
  }

  uart_println("[RNG] Device found");

  // Header Type register[6:0] = header layout type
  // https://wiki.osdev.org/PCI#Header_Type_Register
  if ((pci_get_header_type(&rng_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[RNG]: Unexpected header type");
    return;
  }

  pci_assign_bars(&rng_dev.pci);
  pci_enable_device(&rng_dev.pci);

  virtio_parse_capabilities(&rng_dev.pci, &rng_dev.pci_caps);

  /* VirtIO Device Init Sequence
   * All register accesses go through the MMIO layer (PA → upper half VA). */
  uintptr_t base = rng_dev.pci_caps.common_cfg;

  /* Step 1: Reset Device */
  uart_println("[RNG][VIRTIO-INIT][1] Reset Device");
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();

  /* Wait for RESET to complete */
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }
  uart_println("[RNG][VIRTIO-INIT][1] Reset Device Complete");

  /* Step 2: ACK */
  uart_println("[RNG][VIRTIO-INIT][2] Ack");
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: Set Driver status */
  uart_println("[RNG][VIRTIO-INIT][3] Driver Status");
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: Feature Negotiation */
  uart_println("[RNG][VIRTIO-INIT][4] Negotiate Features");

  /* Read device features - low 32 bits */
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[0]: %x\n", feat_lo);

  /* Read device features - high 32 bits */
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[1]: %x\n", feat_hi);

  /*
   * The RNG device has no device-specific features.
   * accept VIRTIO_F_VERSION_1 (bit 32, in feat_hi bit 0).
   */
  uint32_t guest_lo = 0;
  uint32_t guest_hi = feat_hi & 0x01;

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
  dsb_sy();

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
  dsb_sy();

  uart_printf(" Accepted Features: lo=%x hi=%x\n", guest_lo, guest_hi);

  /* Step 5: FEATURES_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();

  /* Step 6: Re-read and verify FEATURES_OK stuck */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  uart_printf("[RNG] Status: %x\n", (uint32_t)status);

  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[RNG] FEATURES_OK failed — device rejected features");
    return;
  }

  uart_println("[RNG] FEATURES_OK !");

  return;
}
