#ifndef PCI_VIRTIO_H
#define PCI_VIRTIO_H

#include <stdint.h>

/* Device status bits */
#define VIRTIO_STATUS_RESET 0
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

struct virtio_pci_common_cfg {
  /* About the whole device */
  uint32_t device_feature_select; // 0x00 - write 0 or 1 to select feature bits
                                  // [0..31] or [32..63]
  uint32_t device_feature;        // 0x04 - read: device's feature bits
  uint32_t driver_feature_select;
  uint32_t driver_feature; // 0x0C - write: driver's accepted features
  uint16_t msix_config;
  uint16_t num_queues;
  uint8_t device_status;
  uint8_t config_generation;

  /* About a specific virtqueue */
  uint16_t queue_select;
  uint16_t queue_size;
  uint16_t queue_msix_vector;
  uint16_t queue_enable;
  uint16_t queue_notify_off; // 0x1E - read: offset for notification
  uint64_t queue_desc;       // 0x20 - write: physical addr of descriptor table
  uint64_t queue_driver;     // 0x28 - write: physical addr of available ring
  uint64_t queue_device;     // 0x30 - write: physical addr of used ring
} __attribute__((packed));

#endif