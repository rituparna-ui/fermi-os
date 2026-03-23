#ifndef PCI_VIRTIO_H
#define PCI_VIRTIO_H

#include <stdint.h>

#include "pci/pci.h"

/* Device status bits */
#define VIRTIO_STATUS_RESET 0
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

struct virtio_pci_caps {
  uintptr_t common_cfg;  // cfg type 1
  uintptr_t notify_base; // cfg type 2
  uintptr_t isr_cfg;     // cfg type 3
  uintptr_t device_cfg;  // cfg type 4

  /* notify cap extra field (virtio spec §4.1.4.4)
   * notify address for queue i =
   *   notify_base + queue_notify_off[i] * notify_off_multiplier */
  uint32_t notify_off_multiplier;
};

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
};

void virtio_parse_pci_capabilities(struct pci_device *dev,
                                   struct virtio_pci_caps *caps);

#endif
