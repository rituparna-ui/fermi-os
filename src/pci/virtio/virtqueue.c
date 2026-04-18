#include "virtqueue.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "pci/virtio/virtio.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"
#include "utils/utils.h"

int virtqueue_setup(uintptr_t base, uint16_t queue_idx, struct virtqueue *vq,
                    struct virtio_pci_caps *caps) {
  /* Disable MSI-X for config changes (polling mode) */
  mmio_write16(base + VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);
  dsb_sy();

  /* Select queue */
  mmio_write16(base + VIRTIO_COMMON_Q_SELECT, queue_idx);
  dsb_sy();

  uint16_t max_size = mmio_read16(base + VIRTIO_COMMON_Q_SIZE);
  uart_printf("[VQ] Queue %d max size: %d\n", (uint32_t)queue_idx,
              (uint32_t)max_size);

  if (max_size == 0) {
    uart_errorln("[VQ] Queue not available");
    return EERROR;
  }

  uint16_t qsize = VIRTQ_MAX_SIZE;
  if (qsize > max_size)
    qsize = max_size;

  mmio_write16(base + VIRTIO_COMMON_Q_SIZE, qsize);

  /* Disable MSI-X for this queue (polling) */
  mmio_write16(base + VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
  dsb_sy();

  memset(vq->desc, 0, sizeof(struct virtq_desc) * qsize);
  memset(vq->avail, 0, sizeof(struct virtq_avail));
  memset(vq->used, 0, sizeof(struct virtq_used));

  /* Give physical addresses to device (as two 32-bit halves).
   * The pointers in vq are kernel VAs — convert to PA for DMA. */
  uint64_t desc_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->desc);
  mmio_write32(base + VIRTIO_COMMON_Q_DESCLO, (uint32_t)(desc_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DESCHI, (uint32_t)(desc_pa >> 32));

  uint64_t avail_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->avail);
  mmio_write32(base + VIRTIO_COMMON_Q_DRIVERLO,
               (uint32_t)(avail_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DRIVERHI, (uint32_t)(avail_pa >> 32));

  uint64_t used_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->used);
  mmio_write32(base + VIRTIO_COMMON_Q_DEVICELO,
               (uint32_t)(used_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DEVICEHI, (uint32_t)(used_pa >> 32));
  dsb_sy();

  /* Compute notification address for this queue */
  uint16_t notify_off = mmio_read16(base + VIRTIO_COMMON_Q_NOFF);
  vq->notify_addr =
      caps->notify_base + (notify_off * caps->notify_off_multiplier);

  uart_printf("[VQ] Notify offset=%d addr=%x\n", (uint32_t)notify_off,
              (uint64_t)vq->notify_addr);

  vq->size = qsize;
  vq->free_head = 0;
  vq->last_used = 0;

  /* Enable queue */
  mmio_write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
  dsb_sy();

  uart_printf("[VQ] Queue %d enabled (size=%d)\n", (uint32_t)queue_idx,
              (uint32_t)qsize);
  return ESUCCESS;
}

void virtqueue_submit(struct virtqueue *vq, uint64_t buf_pa, uint32_t len,
                      uint16_t flags) {
  uint16_t idx = vq->free_head;
  vq->free_head = (idx + 1) % vq->size;

  vq->desc[idx].addr = buf_pa;
  vq->desc[idx].len = len;
  vq->desc[idx].flags = flags;
  vq->desc[idx].next = 0;

  uint16_t avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->size] = idx;
  dsb_sy();

  vq->avail->idx = avail_idx + 1;
  dsb_sy();
}

void virtqueue_notify(struct virtqueue *vq) {
  mmio_write32(vq->notify_addr, 0);
}

uint32_t virtqueue_poll(struct virtqueue *vq) {
  /* Busy wait until device increments used.idx */
  while (*(volatile uint16_t *)&vq->used->idx == vq->last_used) {
  }
  dsb_sy();

  uint16_t used_idx = (vq->last_used) % vq->size;
  uint32_t written = vq->used->ring[used_idx].len;
  vq->last_used++;

  return written;
}

uint16_t virtqueue_submit_chain(struct virtqueue *vq,
                                const struct virtq_seg *segs, uint16_t n) {
  uint16_t head = vq->free_head;

  for (uint16_t i = 0; i < n; i++) {
    uint16_t idx = (head + i) % vq->size;
    uint16_t flags = segs[i].flags;
    if (i < n - 1)
      flags |= VIRTQ_DESC_F_NEXT;

    vq->desc[idx].addr = segs[i].pa;
    vq->desc[idx].len = segs[i].len;
    vq->desc[idx].flags = flags;
    vq->desc[idx].next = (i < n - 1) ? ((head + i + 1) % vq->size) : 0;
  }

  vq->free_head = (head + n) % vq->size;

  uint16_t avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->size] = head;
  dsb_sy();

  vq->avail->idx = avail_idx + 1;
  dsb_sy();

  return head;
}
