#ifndef HYP_VIRTIO_NET_H
#define HYP_VIRTIO_NET_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal virtio-mmio NETWORK device (virtio 1.x, modern transport), emulated
 * at EL2. The meatiest virtio device here: TWO virtqueues (RX = queue 0,
 * TX = queue 1) routed by QueueSel, and a 12-byte virtio-net header prepended
 * to every packet. A device-config region exposes a fixed MAC.
 *
 * The hypervisor acts as a LOOPBACK NIC: a frame the guest transmits on the TX
 * queue is copied straight into a buffer the guest pre-posted on the RX queue,
 * and BOTH are completed (TX buffer reclaimed, RX buffer delivered) with the
 * RX interrupt injected. So a guest that sends a packet receives it back —
 * proving the full bidirectional, two-queue packet path.
 * ------------------------------------------------------------------------- */

#define VIRTIO_NET_MMIO_BASE 0x0A002000ULL /* distinct window from rng/blk */
#define VIRTIO_NET_MMIO_SIZE 0x1000ULL
#define VIRTIO_NET_SPI       43            /* distinct from rng 41 / blk 42 */

#define VIRTIO_NET_HDR_LEN   12            /* struct virtio_net_hdr (modern) */
#define VIRTIO_NET_RXQ       0
#define VIRTIO_NET_TXQ       1

int  virtio_net_mmio_is_target(uint64_t ipa);
void virtio_net_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                             int size_bytes);

#endif /* HYP_VIRTIO_NET_H */
