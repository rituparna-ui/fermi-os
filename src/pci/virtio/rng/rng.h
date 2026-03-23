#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include "pci/pci.h"

#define VIRTIO_RNG_VENDOR_ID 0x1AF4
#define VIRTIO_RNG_DEVICE_ID 0x1044

struct virtio_rng {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
};

void pci_virtio_rng_init(void);

#endif