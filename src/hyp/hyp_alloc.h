#ifndef HYP_ALLOC_H
#define HYP_ALLOC_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Hypervisor-private page allocator.
 *
 * The hypervisor CANNOT use the guest's PMM (src/mm/pmm) — that allocator is
 * part of the guest image, lives at guest VAs, and manages the guest's RAM
 * (0x40000000..0x240000000). The hyp needs its own pages (stage-2 tables,
 * vGIC state, etc.) carved from its OWN reserved region.
 *
 * That region is everything above the hyp image up to the top of RAM:
 *   [ ALIGN_UP(__hyp_end, 4K) , HYP_RAM_TOP )
 * which, with QEMU '-m 9G' (RAM 0x40000000..0x280000000) and the hyp linked
 * at 0x250000000, is roughly 0x250013000..0x280000000 ≈ 765 MiB — far more
 * than the handful of pages the hypervisor ever needs. It is ABOVE the guest's
 * 8 GiB, so the guest PMM never hands these pages out.
 *
 * This is a pure bump allocator: there is no free(). The hypervisor allocates
 * its fixed control structures once at boot and never releases them.
 * ------------------------------------------------------------------------- */

#define HYP_PAGE_SIZE 4096ULL

/* Top of usable RAM = guest RAM base (0x40000000) + 9 GiB (must match the
 * QEMU '-m 9G' in the Makefile). */
#define HYP_RAM_TOP 0x280000000ULL

/* Allocate `pages` contiguous zeroed 4 KiB pages. Returns the physical base
 * (== VA, EL2 MMU is off) or panics if the reserved region is exhausted. */
void *hyp_alloc_pages(uint64_t pages);

/* Allocate `pages` contiguous zeroed pages whose base is aligned to `align`
 * bytes (align must be a power of two, >= HYP_PAGE_SIZE). Used for the
 * concatenated stage-2 L1 root which needs 8 KiB alignment. */
void *hyp_alloc_aligned(uint64_t pages, uint64_t align);

/* Clean the data cache to PoC over [start, start+len). Needed before the
 * stage-2 table walker (which reads tables as WB-cacheable per VTCR_EL2)
 * first walks tables the hyp built with its own MMU/caches off. No-op-ish on
 * QEMU (no cache model) but architecturally required on real hardware. */
void hyp_dcache_clean_range(uint64_t start, uint64_t len);

#endif /* HYP_ALLOC_H */
