#include "rng.h"
#include "uart/uart.h"

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

  if (pci_get_header_type(&rng_dev.pci) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[RNG]: Unexpected header type");
    return;
  }

  pci_assign_bars(&rng_dev.pci);
  pci_enable_device(&rng_dev.pci);

  return;
}