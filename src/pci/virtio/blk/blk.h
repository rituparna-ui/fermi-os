#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"

/* device cfg layout */
#define VIRTIO_BLK_CFG_CAPACITY 0x00

#define VIRTIO_BLK_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1042

struct virtio_blk {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue vq;
  uint64_t capacity_sectors;
};

void pci_virtio_blk_init(void);

#endif