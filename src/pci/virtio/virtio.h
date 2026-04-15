#ifndef VIRTIO_H
#define VIRTIO_H

#include "pci/pci.h"
#include <stdint.h>

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