#include "rng.h"
#include "pci/pci.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "virtio.h"

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

  if ((pci_get_header_type(&rng_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[RNG]: Unexpected header type");
    return;
  }

  pci_assign_bars(&rng_dev.pci);
  pci_enable_device(&rng_dev.pci);
  virtio_parse_pci_capabilities(&rng_dev.pci, &rng_dev.pci_caps);

  // Virtio Device Init Sequence
  volatile struct virtio_pci_common_cfg *common =
      (volatile struct virtio_pci_common_cfg *)rng_dev.pci_caps.common_cfg;

  /* Reset Device */
  uart_println("[RNG][VIRTIO-INIT][1] Reset Device");
  common->device_status = VIRTIO_STATUS_RESET;
  dsb_sy();

  /* Wait for RESET to complete */
  while (common->device_status != 0) {
  }
  uart_println("[RNG][VIRTIO-INIT][1] Reset Device Complete");

  /* ACK */
  uart_println("[RNG][VIRTIO-INIT][2] Ack");
  common->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
  dsb_sy();

  /* Set Driver status */
  uart_println("[RNG][VIRTIO-INIT][3] Driver Status");
  common->device_status |= VIRTIO_STATUS_DRIVER;
  dsb_sy();

  /* Step 4: Feature Negotiation */
  uart_println("[RNG][VIRTIO-INIT][4] Negotiate Features");

  /* Read device features - low 32 bits */
  common->device_feature_select = 0;
  dsb_sy();
  uint32_t feat_lo = common->device_feature;
  uart_puts(" Device features[0]: ");
  uart_puthex(feat_lo);
  uart_println("");

  /* Read device features - high 32 bits */
  common->device_feature_select = 1;
  dsb_sy();
  uint32_t feat_hi = common->device_feature;
  uart_puts(" Device features[1]: ");
  uart_puthex(feat_hi);
  uart_println("");

  /*
   * The RNG device has no device-specific features.
   * accept VIRTIO_F_VERSION_1 (bit 32, in feat_hi bit 0).
   */
  uint32_t guest_lo = 0;
  uint32_t guest_hi = feat_hi & 0x01;

  common->driver_feature_select = 0;
  dsb_sy();

  common->driver_feature = guest_lo;
  dsb_sy();

  common->driver_feature_select = 1;
  dsb_sy();

  common->driver_feature = guest_hi;
  dsb_sy();

  uart_puts(" Accepted Features: lo=");
  uart_puthex(guest_lo);
  uart_puts(" hi=");
  uart_puthex(guest_hi);
  uart_println("");

  /* FEATURES_OK */
  common->device_status |= VIRTIO_STATUS_FEATURES_OK;
  dsb_sy();

  if (!(common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[RNG] FEATURES_OK failed");
    return;
  }
  uart_puts("[RNG] Status: ");
  uart_puthex(common->device_status);
  uart_println("");

  if (!(common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[RNG] FEATURE_NOT_OK");
    return;
  } else {
    uart_println("[RNG] FEATURE_OK !");
  }

  return;
}