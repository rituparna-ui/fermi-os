#include "rng.h"
#include "pci/pci.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "virtio.h"

#define VIRTQ_SIZE 16

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VIRTQ_SIZE];
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[VIRTQ_SIZE];
};

/* actual memory */
static struct virtq_desc desc[VIRTQ_SIZE] __attribute__((aligned(4096)));
static struct virtq_avail avail __attribute__((aligned(4096)));
static struct virtq_used used __attribute__((aligned(4096)));

static uint8_t rand_byte;

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

  /* Disable MSI-X for config changes (we poll, no interrupts) */
  common->msix_config = 0xFFFF; /* VIRTIO_MSI_NO_VECTOR */
  dsb_sy();

  /* configure queue 0 */
  common->queue_select = 0;
  dsb_sy();

  uint16_t max_size = common->queue_size;
  uart_puts("  Max queue size: ");
  uart_putdec(max_size);
  uart_puts("\n");

  uint16_t qsize = VIRTQ_SIZE;
  if (qsize > max_size)
    qsize = max_size;

  common->queue_size = qsize;

  /* Disable MSI-X for this queue (we poll) */
  common->queue_msix_vector = 0xFFFF; /* VIRTIO_MSI_NO_VECTOR */
  dsb_sy();

  // every queue has a notification address
  uint16_t notify_off = common->queue_notify_off;
  uintptr_t notify_addr =
      rng_dev.pci_caps.notify_base +
      (common->queue_notify_off * rng_dev.pci_caps.notify_off_multiplier);

  uart_puts("  Notify offset=");
  uart_putdec(notify_off);
  uart_puts(" addr=");
  uart_puthex(notify_addr);
  uart_puts("\n");

  /* Give addresses to device */
  common->queue_desc = (uint64_t)&desc;
  common->queue_driver = (uint64_t)&avail;
  common->queue_device = (uint64_t)&used;
  dsb_sy();

  /* Enable queue */
  common->queue_enable = 1;
  dsb_sy();
  uart_println("[RNG] Queue enabled");

  // step 7
  common->device_status |= VIRTIO_STATUS_DRIVER_OK;
  dsb_sy();

  uart_println("[RNG] DRIVER_OK set");

  uart_println("[RNG] Submitting request");

  /* descriptor 0 → buffer */
  desc[0].addr = (uint64_t)&rand_byte;
  desc[0].len = 1;
  desc[0].flags = 2; // VIRTQ_DESC_F_WRITE
  desc[0].next = 0;

  /* put descriptor 0 into avail ring */
  avail.ring[0] = 0;
  avail.idx = 1;

  dsb_sy();
  *(volatile uint32_t *)notify_addr = 0; // queue 0

  uart_println("[RNG] Waiting for device...");

  while (*(volatile uint16_t *)&used.idx == 0) {
  }

  dsb_sy();

  uart_println("[RNG] Got response!");
  uart_puts("Random byte: ");
  uart_puthex(rand_byte);
  uart_println("");
  return;
}