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

/* Send a raw Ethernet frame (Dst MAC + Src MAC + ethertype + payload).
 * The driver prepends the virtio_net_hdr internally; the caller does not
 * include it in `frame`. Returns the number of bytes accepted by the
 * device (== len on success), or a negative value on error.
 *
 * Synchronous: blocks (polls) until the device acks the descriptor chain
 * via the used ring. */
int net_tx(const void *frame, uint32_t len);

/* Hand-crafted ARP request to QEMU's slirp gateway (10.0.2.2). Used as a
 * smoke test that the TX path is actually reaching the device. Once V3
 * lands, the ARP reply will be observable on the RX queue. */
int net_send_arp_probe(void);

/* Drain one received Ethernet frame (without the virtio_net_hdr) into
 * `dst`, copying at most `max_len` bytes. The buffer is automatically
 * re-armed onto the RX queue afterwards.
 *
 *   > 0 : number of payload bytes copied
 *   = 0 : nothing pending
 *   < 0 : error (e.g. unmapped descriptor) */
int net_rx_poll(void *dst, uint32_t max_len);

/* Render a /proc-style multi-line snapshot of the device state into `buf`.
 * Returns bytes written. Snapshot includes MAC, link status, IPv4 stub
 * config, learned gateway MAC, and packet counters. */
int net_get_info(char *buf, uint32_t buflen);

/* Send an ICMP echo request to the slirp gateway (10.0.2.2). Requires the
 * gateway MAC to be learned (via an earlier ARP exchange). Caller picks
 * the seq number; identifier is fixed at 42. Returns bytes sent or a
 * negative value on error. */
int net_send_ping(uint16_t seq);

/* IPv4 + DHCP state. Defined in net.c; populated by dhcp_acquire() at
 * boot. Initialised to slirp defaults so callers (ARP/ICMP/get_info)
 * always see something sane. */
extern uint8_t  g_my_ip[4];
extern uint8_t  g_subnet_mask[4];
extern uint8_t  g_gateway_ip[4];
extern uint8_t  g_dhcp_server[4];
extern uint32_t g_lease_secs;
extern uint8_t  g_dhcp_acquired;

/* Run a synchronous DHCP DISCOVER → OFFER → REQUEST → ACK exchange against
 * the slirp DHCP server. On success commits the lease into the globals
 * above and returns 0; otherwise leaves them untouched and returns -1. */
int dhcp_acquire(void);

#endif
