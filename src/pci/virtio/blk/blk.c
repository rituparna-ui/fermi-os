#include "blk.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

struct virtio_blk blk_dev;

/* Page-aligned backing memory for the virtqueue */
static struct virtq_desc blk_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail blk_avail __attribute__((aligned(4096)));
static struct virtq_used blk_used __attribute__((aligned(4096)));

void pci_virtio_blk_init(void) {
  uart_println("[BLK] Initializing Device");

  /* Step 0: Find device */
  if (!pci_find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID,
                       &blk_dev.pci)) {
    uart_errorln("[BLK] Device not found");
    return;
  }

  uart_println("[BLK] Device found");

  // Header Type register[6:0] = header layout type
  // https://wiki.osdev.org/PCI#Header_Type_Register
  if ((pci_get_header_type(&blk_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[BLK]: Unexpected header type");
    return;
  }

  pci_assign_bars(&blk_dev.pci);
  pci_enable_device(&blk_dev.pci);
  virtio_parse_capabilities(&blk_dev.pci, &blk_dev.pci_caps);

  /* VirtIO Device Init Sequence
   * All register accesses go through the MMIO layer. */
  uintptr_t base = blk_dev.pci_caps.common_cfg;

  /* Step 1: Reset Device */
  uart_println("[BLK][VIRTIO-INIT][1] Reset Device");
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();

  /* Wait for RESET to complete */
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }
  uart_println("[BLK][VIRTIO-INIT][1] Reset Device Complete");

  /* Step 2: ACK */
  uart_println("[BLK][VIRTIO-INIT][2] Ack");
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: Set Driver status */
  uart_println("[BLK][VIRTIO-INIT][3] Driver Status");
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: Feature Negotiation */
  uart_println("[BLK][VIRTIO-INIT][4] Negotiate Features");

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

  /* accept only VIRTIO_F_VERSION_1 (bit 32, in feat_hi bit 0) */
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
    uart_errorln("[BLK] FEATURES_OK failed");
    return;
  }
  uart_printf("[BLK] Status: %x\n", (uint32_t)status);
  uart_println("[BLK] FEATURES_OK !");

  /* Step 6: Setup virtqueue 0 */
  blk_dev.vq.desc = blk_desc;
  blk_dev.vq.avail = &blk_avail;
  blk_dev.vq.used = &blk_used;

  if (virtqueue_setup(base, 0, &blk_dev.vq, &blk_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[BLK] Virtqueue setup failed");
    return;
  }

  /* Step 7: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();
  uart_println("[BLK] DRIVER_OK set");

  /* Read capacity from device config (u64, sectors) */
  uintptr_t dcfg = blk_dev.pci_caps.device_cfg;
  uint32_t cap_lo = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY);
  uint32_t cap_hi = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY + 4);
  blk_dev.capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
  uart_printf("[BLK] Capacity: %d sectors (%d MiB)\n",
              (uint64_t)blk_dev.capacity_sectors,
              (uint64_t)(blk_dev.capacity_sectors / 2048));

  return;
}

int blk_read(uint64_t sector, void *buf) {
  static struct virtio_blk_req hdr __attribute__((aligned(16)));
  static volatile uint8_t status __attribute__((aligned(16)));

  hdr.type = VIRTIO_BLK_T_IN;
  hdr.reserved = 0;
  hdr.sector = sector;
  status = 0xFF;

  struct virtq_seg segs[3] = {
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&hdr), sizeof(hdr),
       VIRTQ_DESC_F_NONE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)buf), VIRTIO_BLK_SECTOR_SIZE,
       VIRTQ_DESC_F_WRITE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&status), 1, VIRTQ_DESC_F_WRITE},
  };

  virtqueue_submit_chain(&blk_dev.vq, segs, 3);
  virtqueue_notify(&blk_dev.vq);
  virtqueue_poll(&blk_dev.vq);

  if (status != VIRTIO_BLK_S_OK) {
    uart_printf("[BLK] read sector %d failed: status=%x\n", sector,
                (uint32_t)status);
    return EERROR;
  }
  return ESUCCESS;
}

int blk_write(uint64_t sector, const void *buf) {
  static struct virtio_blk_req hdr __attribute__((aligned(16)));
  static volatile uint8_t status __attribute__((aligned(16)));

  hdr.type = VIRTIO_BLK_T_OUT;
  hdr.reserved = 0;
  hdr.sector = sector;
  status = 0xFF;

  struct virtq_seg segs[3] = {
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&hdr), sizeof(hdr),
       VIRTQ_DESC_F_NONE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)buf), VIRTIO_BLK_SECTOR_SIZE,
       VIRTQ_DESC_F_NONE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&status), 1, VIRTQ_DESC_F_WRITE},
  };

  virtqueue_submit_chain(&blk_dev.vq, segs, 3);
  virtqueue_notify(&blk_dev.vq);
  virtqueue_poll(&blk_dev.vq);

  if (status != VIRTIO_BLK_S_OK) {
    uart_printf("[BLK] write sector %d failed: status=%x\n", sector,
                (uint32_t)status);
    return EERROR;
  }
  return ESUCCESS;
}
