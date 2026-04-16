#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <stdint.h>

#include "pci/virtio/virtio.h"

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT 1  /* buffer continues via 'next' field */
#define VIRTQ_DESC_F_WRITE 2 /* device writes (vs reads) */

/* MSI-X: no vector assigned (polling mode) */
#define VIRTIO_MSI_NO_VECTOR 0xFFFF

/* Max descriptors per queue */
#define VIRTQ_MAX_SIZE 16

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VIRTQ_MAX_SIZE];
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[VIRTQ_MAX_SIZE];
};

/* complete virtqueue */
struct virtqueue {
  /* negotiated queue size */
  uint16_t size;
  /* next free descriptor index */
  uint16_t free_head;
  /* last used.idx we've seen */
  uint16_t last_used;

  /* PA of notification doorbell */
  uintptr_t notify_addr;

  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
};

/*
 * virtqueue_setup — configure a virtqueue via the common config MMIO registers.
 *   common_cfg_base: PA of VirtIO common config
 *   queue_idx:       which queue to configure (0 for RNG)
 *   vq:              output — filled with queue state
 *   caps:            VirtIO PCI caps (for notify_base + multiplier)
 */

int virtqueue_setup(uintptr_t common_cfg_base, uint16_t queue_idx,
                    struct virtqueue *vq, struct virtio_pci_caps *caps);

/*
 * virtqueue_submit — add a single buffer to the available ring.
 *   buf_pa: physical address of the buffer
 *   len:    buffer length in bytes
 *   flags:  descriptor flags (VIRTQ_DESC_F_WRITE for device→driver)
 */
void virtqueue_submit(struct virtqueue *vq, uint64_t buf_pa, uint32_t len,
                      uint16_t flags);
/* ring the doorbell */
void virtqueue_notify(struct virtqueue *vq);
/* spin until the device produces a used entry.
 * Returns the number of bytes written by the device. */
uint32_t virtqueue_poll(struct virtqueue *vq);

#endif
