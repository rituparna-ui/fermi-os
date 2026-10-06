#ifndef MM_VMA_H
#define MM_VMA_H

#include "mm/mmu/mmu.h" /* VMA_PROT_* / PAGE_SIZE */
#include <stdint.h>
#include <stddef.h>

/*
 * VMA — Virtual Memory Area
 *
 * A VMA is the kernel's bookkeeping record for a contiguous range of user
 * virtual addresses that shares one set of protection flags. It answers the
 * two questions the page-fault path must ask:
 *
 *   1. "Is this faulting address inside a region the task legitimately owns?"
 *   2. "What protection should the new PTEs carry?"
 *
 * Ranges are half-open: [start, end). Both are page-aligned. A zero-length
 * mapping is rejected at creation time, so start < end always holds.
 *
 * Ownership of backing store:
 *   - Every VMA records the physical page backing each of its pages, or NULL
 *     if not yet faulted in. That array is what makes munmap and task reap
 *     able to return exactly what was pmm_allocate'd — nothing leaks, nothing
 *     is freed twice.
 *
 * Splitting and merging exist because POSIX munmap/mprotect accept partial
 * ranges. Rather than refuse them (which the first implementation did),
 * vma_split_at() cuts a VMA in two and vma_merge_adjacent() welds neighbours
 * back together so a long run of alloc/free doesn't burn through the 32 slots.
 */

#define VMA_MAX_PER_TASK 32

/* prot bits are the VMA_PROT_* values defined in mm/mmu/mmu.h — deliberately
 * identical to the Linux/POSIX PROT_* constants so user space can pass its
 * <sys/mman.h> values straight through with no translation table. */

/* vma->flags bits */
#define VMA_ANONYMOUS 0x1 /* zero-fill on fault; no storage backing */
#define VMA_SHARED    0x2 /* future: MAP_SHARED vs MAP_PRIVATE */

typedef struct vma {
  uint64_t start;    /* page-aligned, inclusive */
  uint64_t end;      /* page-aligned, exclusive */
  uint32_t prot;     /* VMA_PROT_* bitmask */
  uint32_t flags;    /* VMA_ANONYMOUS | VMA_SHARED */

  /* Physical pages backing [start, end), indexed by (va - start) / PAGE_SIZE.
   * NULL entry = not yet faulted in (legal for anonymous VMAs).
   *
   * NOTE: after a split the array may be longer than (end - start) /
   * PAGE_SIZE; only the first (end - start) / PAGE_SIZE entries are
   * meaningful. The kernel heap has no realloc, and over-allocating here is
   * cheaper than the alternative of copying on every split. */
  uintptr_t *pages;

  struct vma *next; /* unused for now; the set is a packed ordered array */
} vma_t;

/* Per-task VMA list. Embedded by value in task_t so there is exactly one
 * lifetime (tied to the task struct) and no allocation can fail — a VMA list
 * that could not be allocated would be a silent-corruption bug. */
typedef struct vma_set {
  vma_t entries[VMA_MAX_PER_TASK];
  uint8_t count;
} vma_set_t;

/* Initialize an empty list. */
void vma_init(vma_set_t *set);

/* Number of live VMAs. */
uint8_t vma_count(const vma_set_t *set);

/* Find the VMA containing `va`, or NULL. */
vma_t *vma_find(vma_set_t *set, uint64_t va);

/* Find the first VMA overlapping the half-open range [start, end). */
vma_t *vma_find_overlap(vma_set_t *set, uint64_t start, uint64_t end);

/* Carve [start, end) out of the list as one new VMA.
 *
 * Returns the new VMA on success, NULL if the range overlaps an existing VMA,
 * the table is full, or the backing page array could not be allocated (in
 * which case nothing is mutated — the list is left exactly as it was).
 *
 * `prot` and `flags` are stored verbatim. No physical pages are allocated
 * here: anon VMAs fault in lazily. */
vma_t *vma_add(vma_set_t *set, uint64_t start, uint64_t end, uint32_t prot,
               uint32_t flags);

/* Ensure no VMA straddles `va`.
 *
 * If a VMA contains `va` strictly inside it, that VMA is replaced by two that
 * meet at `va`, each carrying its own page array (the tail half's array is a
 * copy of the original's upper entries). Returns 1 on success.
 *
 * Returns 0 — leaving the list completely unmodified — if `va` lands exactly
 * on an existing boundary (nothing to do, which counts as success), on a
 * non-page-aligned address, or if there is no free slot / allocation failed.
 * Callers must treat 0 as "no change happened".
 *
 * Guarantees the failure path is atomic because the page array is allocated
 * before any list entry is touched.
 */
int vma_split_at(vma_set_t *set, uint64_t va);

/* Weld adjacent VMAs together where they are contiguous AND share identical
 * prot and flags. Coalescing matters because an allocator that mmaps and
 * munmaps in a loop would otherwise consume a VMA slot per iteration.
 *
 * Returns the number of merges performed. */
int vma_merge_adjacent(vma_set_t *set);

/* Remove [start, end), splitting the covering VMAs as needed, and release
 * every backing physical page in that range.
 *
 * A range that overlaps nothing is accepted and is a no-op, matching Linux
 * (munmap of an already-unmapped hole succeeds). Callers are responsible for
 * clearing the page-table entries — that is a pure address-range operation
 * and does not need to know how VMAs are shaped.
 *
 * Returns the number of physical pages released, or -1 if a split failed, in
 * which case nothing was modified. */
int64_t vma_unmap_range(vma_set_t *set, uint64_t start, uint64_t end);

/* Apply `prot` to every VMA intersecting [start, end), splitting as needed.
 *
 * Every VMA that intersects the range ends up ENTIRELY inside it afterwards,
 * which is what makes the caller's follow-up address-range PTE rewrite
 * correct. Returns the number of VMAs modified, or -1 if a split failed and
 * nothing was modified. */
int vma_protect_range(vma_set_t *set, uint64_t start, uint64_t end,
                      uint32_t prot);

/* Total faulted-in (resident) pages across all VMAs. Used by /proc. */
uint64_t vma_resident_pages(const vma_set_t *set);

/* Total pages spanned by all VMAs, faulted or not. */
uint64_t vma_mapped_pages(const vma_set_t *set);

/* Free every backing page still recorded as allocated and reset the list.
 * Called from sched_reap for the whole task. */
void vma_destroy(vma_set_t *set);

/* Free every backing page in `vma` (faulted-in ones only) and free its
 * page array. Does not unlink the VMA from the list. */
void vma_release_pages(vma_t *vma);

#endif