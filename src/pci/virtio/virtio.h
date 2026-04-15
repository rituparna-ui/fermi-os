#ifndef VIRTIO_H
#define VIRTIO_H

#include "pci/pci.h"
#include <stdint.h>

/* Device status bits */
#define VIRTIO_STATUS_RESET 0
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

/* VirtIO PCI Common Config register offsets (virtio spec 4.1.4.3)
 * All accesses must go through mmio_read/write at the correct width. */
#define VIRTIO_COMMON_DFSELECT 0x00 /* u32 rw  device feature select */
#define VIRTIO_COMMON_DF 0x04       /* u32 r   device feature */
#define VIRTIO_COMMON_GFSELECT                                                 \
  0x08                                /* u32 rw  driver (guest) feature select \
                                       */
#define VIRTIO_COMMON_GF 0x0C         /* u32 rw  driver (guest) feature */
#define VIRTIO_COMMON_MSIX 0x10       /* u16 rw  msix config vector */
#define VIRTIO_COMMON_NUMQ 0x12       /* u16 r   number of queues */
#define VIRTIO_COMMON_STATUS 0x14     /* u8  rw  device status */
#define VIRTIO_COMMON_CFGGEN 0x15     /* u8  r   config generation */
#define VIRTIO_COMMON_Q_SELECT 0x16   /* u16 rw  queue select */
#define VIRTIO_COMMON_Q_SIZE 0x18     /* u16 rw  queue size */
#define VIRTIO_COMMON_Q_MSIX 0x1A     /* u16 rw  queue msix vector */
#define VIRTIO_COMMON_Q_ENABLE 0x1C   /* u16 rw  queue enable */
#define VIRTIO_COMMON_Q_NOFF 0x1E     /* u16 r   queue notify offset */
#define VIRTIO_COMMON_Q_DESCLO 0x20   /* u32 rw  descriptor table addr (low) */
#define VIRTIO_COMMON_Q_DESCHI 0x24   /* u32 rw  descriptor table addr (high) */
#define VIRTIO_COMMON_Q_DRIVERLO 0x28 /* u32 rw  available ring addr (low) */
#define VIRTIO_COMMON_Q_DRIVERHI 0x2C /* u32 rw  available ring addr (high) */
#define VIRTIO_COMMON_Q_DEVICELO 0x30 /* u32 rw  used ring addr (low) */
#define VIRTIO_COMMON_Q_DEVICEHI 0x34 /* u32 rw  used ring addr (high) */

struct virtio_pci_caps {
  uintptr_t common_cfg;  // cfg type 1
  uintptr_t notify_base; // cfg type 2
  uintptr_t isr_cfg;     // cfg type 3
  uintptr_t device_cfg;  // cfg type 4

  /* notify cap extra field (section 4.1.4.4)
   * notify address for queue i =
   *   notify_base + queue_notify_off[i] * notify_off_multiplier */
  uint32_t notify_off_multiplier;
};

void virtio_parse_capabilities(struct pci_device *dev,
                               struct virtio_pci_caps *caps);

#endif