#include "console.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "strings/strings.h"

/* ---------------------------------------------------------------------------
 * virtio-console (port 0, TX-only path)
 *
 * QEMU exposes this as `-device virtio-serial-pci -device virtconsole,...`.
 * The Makefile wires the backend to a host-side file via:
 *   -chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt
 * Anything we push down the TX virtqueue lands as bytes appended to that
 * file \u2014 a great low-friction logging side-channel separate from the
 * PL011 UART that carries the interactive shell.
 *
 * For the no-MULTIPORT case, only two virtqueues exist:
 *   vq 0: receiveq(0)   host -> guest
 *   vq 1: transmitq(0)  guest -> host   <-- we use this
 * RX is configured (so the device sees a valid two-queue setup) but no
 * buffers are pre-posted; that's fine because we never read.
 * --------------------------------------------------------------------------- */

/* Page-aligned virtqueue backing memory \u2014 same pattern as every other
 * virtio driver in this kernel. */
static struct virtq_desc rx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail rx_avail __attribute__((aligned(4096)));
static struct virtq_used rx_used __attribute__((aligned(4096)));

static struct virtq_desc tx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail tx_avail __attribute__((aligned(4096)));
static struct virtq_used tx_used __attribute__((aligned(4096)));

/* DMA staging buffer for outgoing bytes. Single buffer, single in-flight
 * write \u2014 vcons_send polls before returning so we never have two
 * descriptors in flight. 4 KiB is plenty for any one log line. */
#define CONSOLE_TX_BUF 4096
static uint8_t tx_buf[CONSOLE_TX_BUF] __attribute__((aligned(64)));

static struct virtio_console con_dev;
static int con_ready = 0;

int vcons_send(const void *buf, uint32_t len) {
  if (!con_ready) {
    return -1;
  }
  if (len == 0) {
    return 0;
  }
  /* Fragment large writes so we never overflow tx_buf. The split is
   * transparent to the host \u2014 it just sees a stream of bytes. */
  uint32_t done = 0;
  const uint8_t *src = (const uint8_t *)buf;
  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > CONSOLE_TX_BUF) {
      chunk = CONSOLE_TX_BUF;
    }
    memcpy(tx_buf, src + done, chunk);

    /* virtio-console TX has no header (\u00a75.3.6.4): just raw bytes the host
     * delivers to the chardev backend. The descriptor is device-readable
     * (no VIRTQ_DESC_F_WRITE). */
    uint64_t pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)tx_buf);
    virtqueue_submit(&con_dev.tx_vq, pa, chunk, VIRTQ_DESC_F_NONE);
    virtqueue_notify(&con_dev.tx_vq);
    (void)virtqueue_poll(&con_dev.tx_vq);
    done += chunk;
  }
  return (int)done;
}

void pci_virtio_console_init(void) {
  uart_println("[CONSOLE] Initializing Device");

  if (!pci_find_device(VIRTIO_CONSOLE_VENDOR_ID, VIRTIO_CONSOLE_DEVICE_ID,
                       &con_dev.pci)) {
    uart_println("[CONSOLE] Device not found (skipping)");
    return;
  }
  uart_println("[CONSOLE] Device found");

  if ((pci_get_header_type(&con_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[CONSOLE] Unexpected header type");
    return;
  }

  pci_assign_bars(&con_dev.pci);
  pci_enable_device(&con_dev.pci);
  virtio_parse_capabilities(&con_dev.pci, &con_dev.pci_caps);

  uintptr_t base = con_dev.pci_caps.common_cfg;

  /* 1. Reset */
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }

  /* 2. Ack + 3. Driver */
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* 4. Features. We accept VIRTIO_F_VERSION_1 (bit 32 = feat_hi bit 0)
   * and reject every console-specific feature:
   *   - F_SIZE       (bit 0): the host-set cols/rows; we don't render a TTY
   *   - F_MULTIPORT  (bit 1): would require a control queue + multi-port
   *                           bookkeeping; out of scope for v1
   *   - F_EMERG_WRITE (bit 2): synchronous write-without-vq for early-boot
   *                            panics. Cool but not yet hooked up. */
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf("[CONSOLE] Device features: hi=%x lo=%x\n", feat_hi, feat_lo);

  uint32_t guest_lo = 0;
  uint32_t guest_hi = feat_hi & 0x01; /* VIRTIO_F_VERSION_1 only */

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
  dsb_sy();

  /* 5. FEATURES_OK + readback */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[CONSOLE] FEATURES_OK rejected");
    return;
  }

  /* 6. Set up both queues. We must configure RX even though we never
   * post buffers \u2014 the spec requires every advertised queue to be set
   * up before DRIVER_OK or the device may refuse to operate. */
  con_dev.rx_vq.desc = rx_desc;
  con_dev.rx_vq.avail = &rx_avail;
  con_dev.rx_vq.used = &rx_used;
  if (virtqueue_setup(base, VIRTIO_CONSOLE_VQ_RX, &con_dev.rx_vq,
                      &con_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[CONSOLE] rx queue setup failed");
    return;
  }

  con_dev.tx_vq.desc = tx_desc;
  con_dev.tx_vq.avail = &tx_avail;
  con_dev.tx_vq.used = &tx_used;
  if (virtqueue_setup(base, VIRTIO_CONSOLE_VQ_TX, &con_dev.tx_vq,
                      &con_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[CONSOLE] tx queue setup failed");
    return;
  }

  /* 7. DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();

  con_ready = 1;
  uart_println("[CONSOLE] DRIVER_OK; tx-only path live");

  /* Boot banner so the host file is non-empty even before the user does
   * anything. Useful for confirming the driver came up at all. */
  static const char banner[] =
      "[Fermi OS] virtio-console attached. Hello from guest!\n";
  vcons_send(banner, sizeof(banner) - 1);
}
