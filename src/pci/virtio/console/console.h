#ifndef VIRTIO_CONSOLE_H
#define VIRTIO_CONSOLE_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"
#include <stdint.h>

/* virtio device id 3 = console; modern PCI device id = 0x1040 + id */
#define VIRTIO_CONSOLE_VENDOR_ID 0x1AF4
#define VIRTIO_CONSOLE_DEVICE_ID 0x1043

/* Virtqueue indices for the (no-MULTIPORT) config: port 0 only.
 *   receiveq[0]  = host -> guest data
 *   transmitq[0] = guest -> host data
 * We currently use only the TX queue \u2014 RX is left unposted, so any
 * input the host writes will queue up but won't be visible to user space
 * until we wire that path. */
#define VIRTIO_CONSOLE_VQ_RX 0
#define VIRTIO_CONSOLE_VQ_TX 1

/* Device-cfg layout for non-MULTIPORT virtio-console (\u00a75.3.4). We don't
 * read any of these today \u2014 the host's terminal emulator (or in our case
 * a host-side log file) decides how wide things look. */
#define VIRTIO_CONSOLE_CFG_COLS         0x00 /* le16 */
#define VIRTIO_CONSOLE_CFG_ROWS         0x02 /* le16 */
#define VIRTIO_CONSOLE_CFG_MAX_NR_PORTS 0x04 /* le32 (only with MULTIPORT) */
#define VIRTIO_CONSOLE_CFG_EMERG_WR     0x08 /* le32 (only with EMERG_WRITE) */

struct virtio_console {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue tx_vq;
  struct virtqueue rx_vq;
};

/* Bring the device up. Safe to call once at boot; failures are logged
 * and leave the driver in a !ready state, in which case vcons_send()
 * silently returns -1. */
void pci_virtio_console_init(void);

/* Push `len` bytes onto the TX virtqueue and ring the doorbell. The host
 * sees the bytes on the chardev backend wired up in the QEMU command
 * line (`-chardev file,id=vc,path=...` in our Makefile). Returns the
 * number of bytes accepted (always == len on success), or -1 if the
 * driver is not ready. Polls for completion \u2014 same model as rng/blk. */
int vcons_send(const void *buf, uint32_t len);

#endif
