#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"

#define VIRTIO_NET_VENDOR_ID 0x1AF4
/* Modern (non-transitional) virtio-net device id. The transitional id is
 * 0x1000; QEMU emits 0x1041 when the disable-legacy=on flag is set. */
#define VIRTIO_NET_DEVICE_ID 0x1041

/* Device-cfg layout (VirtIO 1.x §5.1.4) */
#define VIRTIO_NET_CFG_MAC    0x00 /* 6 bytes */
#define VIRTIO_NET_CFG_STATUS 0x06 /* 2 bytes — only valid with F_STATUS */

/* Link-status bits (config.status) */
#define VIRTIO_NET_S_LINK_UP  (1 << 0)
#define VIRTIO_NET_S_ANNOUNCE (1 << 1)

/* Feature bits we care about (low 32) */
#define VIRTIO_NET_F_MAC      (1U << 5)
#define VIRTIO_NET_F_STATUS   (1U << 16)

/* virtio_net_hdr — prepended to every TX/RX frame on the wire side of the
 * virtqueue. Size depends on which features are negotiated. With
 * VIRTIO_F_VERSION_1 (always for modern), num_buffers is always present and
 * the header is 12 bytes. */
struct virtio_net_hdr {
  uint8_t  flags;
  uint8_t  gso_type;
  uint16_t hdr_len;
  uint16_t gso_size;
  uint16_t csum_start;
  uint16_t csum_offset;
  uint16_t num_buffers;
} __attribute__((packed));

#define VIRTIO_NET_HDR_LEN ((uint32_t)sizeof(struct virtio_net_hdr))

/* Queue indices */
#define VIRTIO_NET_QUEUE_RX 0
#define VIRTIO_NET_QUEUE_TX 1

struct virtio_net {
  struct pci_device      pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue       rx_vq;
  struct virtqueue       tx_vq;
  uint8_t                mac[6];
  uint8_t                have_mac;
  uint16_t               link_status;
  uint8_t                have_status;
};

void pci_virtio_net_init(void);

#endif
