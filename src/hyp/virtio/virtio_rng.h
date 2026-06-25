#ifndef HYP_VIRTIO_RNG_H
#define HYP_VIRTIO_RNG_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal virtio-mmio entropy (RNG) device, modern transport (virtio 1.x,
 * Version=2 — NOT legacy), emulated at EL2. A guest discovers it through the
 * standard virtio-mmio register block at a fixed IPA window (left stage-2
 * INVALID so accesses trap, like GICD/GICR), drives the init handshake, sets up
 * one split virtqueue (queue 0) in its own RAM, and writes QueueNotify; the hyp
 * fills the guest's WRITE descriptors with pseudo-random bytes, posts a used-ring
 * element, and injects the device's SPI.
 *
 * Register offsets / values / struct layouts were design-verified against the
 * virtio 1.x MMIO spec. All guest-supplied IPAs (ring bases + every desc.addr)
 * are bounds-checked via vcpu_ipa_to_pa before EL2 touches them (VM-escape
 * defense). Cache coherence is handled explicitly: invalidate before reading
 * guest-written rings, clean after writing buffers/used ring, with the used.idx
 * publish strictly ordered after the element + buffer stores.
 * ------------------------------------------------------------------------- */

/* Device MMIO window (guest IPA). 4 KiB, left stage-2-invalid to trap. */
#define VIRTIO_MMIO_BASE 0x0A000000ULL
#define VIRTIO_MMIO_SIZE 0x1000ULL

/* Virtual SPI INTID injected on a completed buffer (distinct from doorbell 40
 * and the timer PPI 30). The guest enables it in the (emulated) GICD ISENABLER. */
#define VIRTIO_RNG_SPI 41

/* True if `ipa` is in this device's MMIO window. */
int virtio_mmio_is_target(uint64_t ipa);

/* Emulate a trapped virtio-mmio register access. `is_write` selects direction;
 * `val` is the source (write) / destination (read); `size_bytes` the width.
 * On a QueueNotify write this walks the virtqueue, fills buffers, and injects
 * the device SPI. */
void virtio_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                         int size_bytes);

#endif /* HYP_VIRTIO_RNG_H */
