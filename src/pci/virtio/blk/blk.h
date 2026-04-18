#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"

/* device cfg layout */
#define VIRTIO_BLK_CFG_CAPACITY 0x00

#define VIRTIO_BLK_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1042

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Header types */
#define VIRTIO_BLK_T_IN 0

/* status byte values */
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

struct virtio_blk {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue vq;
  uint64_t capacity_sectors;
};

struct virtio_blk_req {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

void pci_virtio_blk_init(void);
int blk_read(uint64_t sector, void *buf);

#endif