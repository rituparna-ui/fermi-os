#include "net.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

/* Page-aligned backing memory for both virtqueues. RX is queue 0, TX is
 * queue 1. Each queue gets its own descriptor table, available ring, and
 * used ring. */
static struct virtq_desc rx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail rx_avail __attribute__((aligned(4096)));
static struct virtq_used  rx_used  __attribute__((aligned(4096)));

static struct virtq_desc tx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail tx_avail __attribute__((aligned(4096)));
static struct virtq_used  tx_used  __attribute__((aligned(4096)));

static struct virtio_net net_dev;

void pci_virtio_net_init(void) {
  uart_println("[NET] Initializing Device");

  /* Step 0: Find device on the PCI bus */
  if (pci_find_device(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID,
                      &net_dev.pci) != ESUCCESS) {
    uart_errorln("[NET] Device not found");
    return;
  }
  uart_println("[NET] Device found");

  if ((pci_get_header_type(&net_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[NET] Unexpected header type");
    return;
  }

  pci_assign_bars(&net_dev.pci);
  pci_enable_device(&net_dev.pci);
  virtio_parse_capabilities(&net_dev.pci, &net_dev.pci_caps);

  /* VirtIO Device Init Sequence (§3.1.1). All register accesses through MMIO. */
  uintptr_t base = net_dev.pci_caps.common_cfg;

  /* Step 1: Reset Device */
  uart_println("[NET][VIRTIO-INIT][1] Reset Device");
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }
  uart_println("[NET][VIRTIO-INIT][1] Reset Device Complete");

  /* Step 2: ACK */
  uart_println("[NET][VIRTIO-INIT][2] Ack");
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS,
              status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: Set Driver status */
  uart_println("[NET][VIRTIO-INIT][3] Driver Status");
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: Feature Negotiation */
  uart_println("[NET][VIRTIO-INIT][4] Negotiate Features");

  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[0]: %x\n", feat_lo);

  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[1]: %x\n", feat_hi);

  /* VIRTIO_F_VERSION_1 (bit 32 → feat_hi bit 0) is required for the modern
   * device path. Refuse to drive a device that doesn't offer it. */
  if (!(feat_hi & 0x01)) {
    uart_errorln("[NET] Device does not advertise VIRTIO_F_VERSION_1");
    return;
  }

  /* Accept MAC and STATUS if offered, plus VERSION_1. Reject every GSO /
   * checksum-offload feature for now — TX/RX paths assume plain frames. */
  uint32_t want_lo = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
  uint32_t guest_lo = feat_lo & want_lo;
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
  mmio_write8(base + VIRTIO_COMMON_STATUS,
              status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();

  /* Step 6a: Re-read and verify FEATURES_OK stuck */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[NET] FEATURES_OK failed");
    return;
  }
  uart_printf("[NET] Status: %x\n", (uint32_t)status);
  uart_println("[NET] FEATURES_OK !");

  /* Step 6b: Setup virtqueues. RX (0) and TX (1). */
  net_dev.rx_vq.desc  = rx_desc;
  net_dev.rx_vq.avail = &rx_avail;
  net_dev.rx_vq.used  = &rx_used;
  if (virtqueue_setup(base, VIRTIO_NET_QUEUE_RX, &net_dev.rx_vq,
                      &net_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[NET] RX virtqueue setup failed");
    return;
  }

  net_dev.tx_vq.desc  = tx_desc;
  net_dev.tx_vq.avail = &tx_avail;
  net_dev.tx_vq.used  = &tx_used;
  if (virtqueue_setup(base, VIRTIO_NET_QUEUE_TX, &net_dev.tx_vq,
                      &net_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[NET] TX virtqueue setup failed");
    return;
  }

  /* Step 7: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();
  uart_println("[NET] DRIVER_OK set");

  /* Read MAC and link status from device cfg. With F_MAC negotiated the
   * device has placed a 6-byte MAC at offset 0; without it we'd have to
   * generate one ourselves. */
  uintptr_t dcfg = net_dev.pci_caps.device_cfg;

  if (guest_lo & VIRTIO_NET_F_MAC) {
    for (int i = 0; i < 6; i++) {
      net_dev.mac[i] = mmio_read8(dcfg + VIRTIO_NET_CFG_MAC + i);
    }
    net_dev.have_mac = 1;
    uart_printf("[NET] MAC: %x:%x:%x:%x:%x:%x\n",
                (uint64_t)net_dev.mac[0], (uint64_t)net_dev.mac[1],
                (uint64_t)net_dev.mac[2], (uint64_t)net_dev.mac[3],
                (uint64_t)net_dev.mac[4], (uint64_t)net_dev.mac[5]);
  } else {
    uart_println("[NET] Device did not advertise VIRTIO_NET_F_MAC");
  }

  if (guest_lo & VIRTIO_NET_F_STATUS) {
    net_dev.link_status = mmio_read16(dcfg + VIRTIO_NET_CFG_STATUS);
    net_dev.have_status = 1;
    uart_printf("[NET] Link: %s (status=%x)\n",
                (net_dev.link_status & VIRTIO_NET_S_LINK_UP) ? "UP" : "DOWN",
                (uint64_t)net_dev.link_status);
  }
}
