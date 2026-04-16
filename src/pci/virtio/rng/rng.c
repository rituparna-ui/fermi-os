#include "rng.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

/* Page-aligned backing memory for the virtqueue */
static struct virtq_desc rng_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail rng_avail __attribute__((aligned(4096)));
static struct virtq_used rng_used __attribute__((aligned(4096)));

/* Buffer for the random byte */
static uint8_t rand_byte;

struct virtio_rng rng_dev;

static void request_byte() {
  /* Submit a request for 1 random byte.
   * The descriptor addr must be a physical address for DMA. */
  uart_println("[RNG] Requesting random byte...");
  uint64_t rand_byte_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)&rand_byte);
  virtqueue_submit(&rng_dev.vq, rand_byte_pa, 1, VIRTQ_DESC_F_WRITE);
  virtqueue_notify(&rng_dev.vq);

  uart_println("[RNG] Waiting for device...");
  virtqueue_poll(&rng_dev.vq);

  uart_println("[RNG] Got response!");
  uart_printf("Random byte: %x\n", (uint32_t)rand_byte);
}

void pci_virtio_rng_init() {
  uart_println("[RNG] Initializing Device");

  /* Step 0: Find device */
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
   * All register accesses go through the MMIO layer. */
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
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[RNG] FEATURES_OK failed");
    return;
  }
  uart_printf("[RNG] Status: %x\n", (uint32_t)status);
  uart_println("[RNG] FEATURES_OK !");

  /* Step 6: Setup virtqueue 0 */
  rng_dev.vq.desc = rng_desc;
  rng_dev.vq.avail = &rng_avail;
  rng_dev.vq.used = &rng_used;

  if (virtqueue_setup(base, 0, &rng_dev.vq, &rng_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[RNG] Virtqueue setup failed");
    return;
  }

  /* Step 7: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();
  uart_println("[RNG] DRIVER_OK set");

  request_byte();

  return;
}
