#ifndef HYP_VIRTIO_BLK_H
#define HYP_VIRTIO_BLK_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal virtio-mmio BLOCK device (virtio 1.x, modern transport), emulated at
 * EL2. A second virtio-mmio device alongside the entropy one, at its own IPA
 * window. It exercises the full virtio request shape that RNG does not:
 *   - a 3-descriptor chain: [ RO header (type + sector) ][ data buffer(s) ][
 *     WO 1-byte status ]
 *   - bidirectional transfer: VIRTIO_BLK_T_IN reads disk->guest buffer;
 *     VIRTIO_BLK_T_OUT writes guest buffer->disk
 *   - a device config region (capacity, at config offset 0x100)
 * The "disk" is a small RAM region carved from the hyp pool (persistent across
 * guest reboots, like a real disk). 512-byte sectors.
 * ------------------------------------------------------------------------- */

#define VIRTIO_BLK_MMIO_BASE 0x0A001000ULL /* distinct from RNG's 0x0A000000 */
#define VIRTIO_BLK_MMIO_SIZE 0x1000ULL
#define VIRTIO_BLK_SPI       42            /* distinct from RNG SPI 41, doorbell 40 */

#define VIRTIO_BLK_SECTOR    512
#define VIRTIO_BLK_NSECTORS  64            /* 32 KiB RAM-backed disk */

/* Reserve the backing disk once at boot (from the hyp pool). */
void virtio_blk_init(void);

int  virtio_blk_mmio_is_target(uint64_t ipa);
void virtio_blk_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                             int size_bytes);

#endif /* HYP_VIRTIO_BLK_H */
