#ifndef VIRTIO_BALLOON_H
#define VIRTIO_BALLOON_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"
#include <stdint.h>

/* Modern virtio device IDs are 0x1040 + virtio device id. Balloon = 5. */
#define VIRTIO_BALLOON_VENDOR_ID 0x1AF4
#define VIRTIO_BALLOON_DEVICE_ID 0x1045

/* Virtio-balloon spec uses *fixed* 4 KiB balloon pages regardless of host
 * page size. PFN = phys_addr / 4096. */
#define VIRTIO_BALLOON_PFN_SHIFT 12

/* Hard cap on how many pages we will inflate. Drives the size of the
 * statically-allocated PFN tracking array — bumping this trades data
 * segment bytes for a larger reachable balloon. 1024 pages = 4 MiB. */
#define VIRTIO_BALLOON_MAX_PAGES 1024

/* Virtqueue indices per spec §5.5.2 */
#define VIRTIO_BALLOON_VQ_INFLATE 0
#define VIRTIO_BALLOON_VQ_DEFLATE 1

/* Device-cfg field offsets (§5.5.4). All little-endian u32. */
#define VIRTIO_BALLOON_CFG_NUM_PAGES 0x00
#define VIRTIO_BALLOON_CFG_ACTUAL    0x04

struct virtio_balloon {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue inflate_vq;
  struct virtqueue deflate_vq;
  uint32_t actual; /* mirrors what we last wrote to device_cfg.actual */
};

/* Bring the device up. Idempotent only in the sense that calling it twice
 * is harmless if the first call failed before driver_ok. */
void pci_virtio_balloon_init(void);

/* Hand `n` PMM pages to the host. Returns the number actually inflated
 * (may be < n if PMM is exhausted or the cap is reached). On failure of
 * the device-side handshake returns -1 and leaves the balloon in its
 * prior state. Pages are pmm_allocate_page'd and tracked internally — do
 * not free them, only deflate. */
int balloon_inflate(uint32_t n);

/* Reclaim `n` pages from the host back into the PMM free pool. Returns
 * the number actually deflated (capped at the current balloon size). */
int balloon_deflate(uint32_t n);

/* Snapshot for /proc and the shell. host_target is what device_cfg.num_pages
 * currently reads — i.e. the host's *wish* for the balloon size. */
void balloon_get_status(uint32_t *actual_pages, uint32_t *host_target);

#endif
