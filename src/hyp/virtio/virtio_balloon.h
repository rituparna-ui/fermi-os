#ifndef HYP_VIRTIO_BALLOON_H
#define HYP_VIRTIO_BALLOON_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal virtio-mmio MEMORY-BALLOON device (virtio 1.x, DeviceID 5), emulated
 * at EL2.
 *
 * HONEST MODEL — fixed stage-2, no runtime unmap. A real balloon hands inflated
 * pages back to the host (MADV_DONTNEED / unmap). THIS hypervisor builds each
 * guest's stage-2 ONCE at boot as a fixed linear map (s2_build_vm2) — there is
 * no demand paging and no runtime unmap — so it CANNOT return a page to the
 * host. It therefore demonstrates the only honest thing it can:
 *   - INFLATE: the guest donates a list of PFNs it promises not to touch. The
 *     device ZEROES each donated page (proving it legitimately reuses the page's
 *     contents) and counts it. The host PA is NOT unmapped or freed — it stays
 *     mapped in stage-2. A guest that later re-reads an inflated page sees the
 *     zeros written at inflate time (cleaned to PoC then).
 *   - DEFLATE: the guest reclaims those PFNs; the device just decrements the
 *     counter (bookkeeping only — it does NOT re-zero or touch memory).
 * Every inflate log line says "NOT host-unmapped; fixed stage-2" so the demo
 * never overclaims. This is the ONLY deviation from a real balloon; the register
 * interface, virtqueue mechanics, config space, config-change IRQ, and
 * ConfigGeneration semantics are all virtio-1.x faithful.
 *
 * Two queues: 0 = inflateq, 1 = deflateq. The driver puts arrays of little-
 * endian u32 PFNs (4 KiB units; PFN_SHIFT = 12, independent of guest page size)
 * on a queue; these are device-READ buffers (no DESC_F_WRITE) — the OPPOSITE
 * direction from the rng device. Config space exposes num_pages (RO to the
 * guest, device-owned: the target balloon size) and actual (RW, driver-owned:
 * pages currently in the balloon). The device SELF-DRIVES: on write-side traps
 * it runs a CNTPCT clock that retargets num_pages between an inflate goal and 0
 * and raises a config-change interrupt (InterruptStatus bit1), so the guest
 * exercises BOTH inflate and deflate with no external trigger. The autopilot
 * fires ONLY on write-side traps (NOTIFY/INT_ACK/STATUS), never on a config
 * read, so the driver's ConfigGeneration read snapshot can never straddle a
 * generation bump.
 * ------------------------------------------------------------------------- */

#define VIRTIO_BALLOON_MMIO_BASE 0x0A004000ULL /* next free virtio window */
#define VIRTIO_BALLOON_MMIO_SIZE 0x1000ULL
#define VIRTIO_BALLOON_SPI       44            /* distinct from rng41/blk42/net43 */

#define VIRTIO_BALLOON_PFN_SHIFT 12  /* PFNs are always 4 KiB units */
#define VIRTIO_BALLOON_PFNS_MAX  256 /* VIRTIO_BALLOON_ARRAY_PFNS_MAX */

void virtio_balloon_init(void); /* called from hyp_main */
int  virtio_balloon_mmio_is_target(uint64_t ipa);
void virtio_balloon_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                                 int size_bytes);

#endif /* HYP_VIRTIO_BALLOON_H */
