#include "vma.h"
#include "mm/heap/heap.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "uart/uart.h"

/*
 * VMA bookkeeping for user address spaces.
 *
 * The list is a packed, address-ordered array of vma_t embedded in the task.
 * Lookup and the common mutate operations are linear scans, which is the
 * right trade at VMA_MAX_PER_TASK = 32: the list is small, always
 * cache-resident, and an rbtree keyed on start would cost more in pointer
 * chasing and insertion complexity than it saves at this scale.
 */

static uint64_t vma_npages(const vma_t *vma) {
  return (vma->end - vma->start) / PAGE_SIZE;
}

void vma_init(vma_set_t *set) {
  if (!set) {
    return;
  }
  set->count = 0;
}

uint8_t vma_count(const vma_set_t *set) {
  return set ? set->count : 0;
}

vma_t *vma_find(vma_set_t *set, uint64_t va) {
  if (!set) {
    return 0;
  }
  for (uint8_t i = 0; i < set->count; i++) {
    vma_t *v = &set->entries[i];
    /* Half-open containment: [start, end). va == end is NOT inside. */
    if (va >= v->start && va < v->end) {
      return v;
    }
  }
  return 0;
}

vma_t *vma_find_overlap(vma_set_t *set, uint64_t start, uint64_t end) {
  if (!set) {
    return 0;
  }
  for (uint8_t i = 0; i < set->count; i++) {
    vma_t *v = &set->entries[i];
    if (start < v->end && v->start < end) {
      return v;
    }
  }
  return 0;
}

/* Insert into the packed, address-ordered entries[] array. Returns the index
 * the new VMA landed at so the caller can hand back a stable pointer into the
 * set (a pointer computed before the shift would dangle or alias a
 * neighbour). */
static uint8_t vma_insert_sorted(vma_set_t *set, const vma_t *vma) {
  uint8_t idx = set->count;
  while (idx > 0 && set->entries[idx - 1].start > vma->start) {
    set->entries[idx] = set->entries[idx - 1];
    idx--;
  }
  set->entries[idx] = *vma;
  set->count++;
  return idx;
}

vma_t *vma_add(vma_set_t *set, uint64_t start, uint64_t end, uint32_t prot,
               uint32_t flags) {
  if (!set || start >= end) {
    return 0;
  }
  if (start % PAGE_SIZE || end % PAGE_SIZE) {
    uart_errorln("[VMA] add: range not page-aligned");
    return 0;
  }
  if (set->count >= VMA_MAX_PER_TASK) {
    uart_errorln("[VMA] add: table full");
    return 0;
  }
  if (vma_find_overlap(set, start, end)) {
    uart_errorln("[VMA] add: range overlaps existing VMA");
    return 0;
  }

  uint64_t npages = (end - start) / PAGE_SIZE;

  /* Guard against a length that would overflow the page-index arithmetic.
   * USER_VA_TOP bounds this in practice; check anyway so a future caller
   * widening the user range gets a clean NULL, not a tiny kmalloc. */
  if (npages > (1ULL << 20)) {
    uart_errorln("[VMA] add: range too large");
    return 0;
  }

  /* Allocating the page array BEFORE mutating the list means every failure
   * path leaves the list untouched. */
  uintptr_t *pages = (uintptr_t *)kmalloc(npages * sizeof(uintptr_t));
  if (!pages) {
    uart_errorln("[VMA] add: kmalloc for page array failed");
    return 0;
  }
  memset(pages, 0, npages * sizeof(uintptr_t));

  vma_t vma;
  vma.start = start;
  vma.end   = end;
  vma.prot  = prot;
  vma.flags = flags;
  vma.pages = pages;
  vma.next  = 0;

  vma_insert_sorted(set, &vma);
  /* Resolve through vma_find rather than returning the address of the local
   * `vma`: the list is sorted, so insertion may have shifted everything. */
  return vma_find(set, start);
}

int vma_split_at(vma_set_t *set, uint64_t va) {
  if (!set) {
    return 0;
  }
  if (va % PAGE_SIZE) {
    return 0;
  }

  vma_t *v = vma_find(set, va);
  if (!v) {
    /* va is in a hole or outside every VMA. Nothing to split — that is a
     * success: the postcondition "no VMA straddles va" already holds. */
    return 1;
  }
  if (v->start == va || v->end == va) {
    return 1; /* already a boundary */
  }

  if (set->count >= VMA_MAX_PER_TASK) {
    uart_errorln("[VMA] split: table full");
    return 0;
  }

  uint64_t head_pages = (va - v->start) / PAGE_SIZE;
  uint64_t tail_pages = (v->end - va) / PAGE_SIZE;

  /* Allocate the tail's array and copy the upper entries over BEFORE
   * mutating anything. On failure the list is untouched, which is what lets
   * munmap/mprotect safely do two splits and then bail out wholesale. */
  uintptr_t *tail_pages_arr = (uintptr_t *)kmalloc(tail_pages * sizeof(uintptr_t));
  if (!tail_pages_arr) {
    uart_errorln("[VMA] split: kmalloc for tail failed");
    return 0;
  }
  memcpy(tail_pages_arr, v->pages + head_pages,
         tail_pages * sizeof(uintptr_t));

  vma_t tail;
  tail.start = va;
  tail.end   = v->end;
  tail.prot  = v->prot;
  tail.flags = v->flags;
  tail.pages = tail_pages_arr;
  tail.next  = 0;

  /* Truncate the head first (v may move on the insert below, so we must not
   * use it afterwards). The head keeps its original array, now over-long but
   * with only its first head_pages entries meaningful. */
  v->end = va;

  vma_insert_sorted(set, &tail);
  return 1;
}

int vma_merge_adjacent(vma_set_t *set) {
  if (!set) {
    return 0;
  }
  int merges = 0;

  for (uint8_t i = 0; i + 1 < set->count;) {
    vma_t *a = &set->entries[i];
    vma_t *b = &set->entries[i + 1];

    /* Only weld truly contiguous neighbours sharing protection; anything else
     * is a real boundary the user asked for. */
    if (a->end != b->start || a->prot != b->prot || a->flags != b->flags) {
      i++;
      continue;
    }

    uint64_t a_pages = (a->end - a->start) / PAGE_SIZE;
    uint64_t b_pages = (b->end - b->start) / PAGE_SIZE;

    /* Concatenate into a fresh array, then release both originals. Allocate
     * first so an OOM leaves the list untouched. */
    uintptr_t *merged = (uintptr_t *)kmalloc((a_pages + b_pages) *
                                             sizeof(uintptr_t));
    if (!merged) {
      /* Can't merge this pair right now. Skip past `a` so we don't spin, and
       * try again on the next pair. */
      i++;
      continue;
    }
    memcpy(merged, a->pages, a_pages * sizeof(uintptr_t));
    memcpy(merged + a_pages, b->pages, b_pages * sizeof(uintptr_t));

    kfree(a->pages);
    kfree(b->pages);

    a->end   = b->end;
    a->pages = merged;

    /* Drop b by shifting the tail down. */
    for (uint8_t j = (uint8_t)(i + 1); j + 1 < set->count; j++) {
      set->entries[j] = set->entries[j + 1];
    }
    set->count--;
    memset(&set->entries[set->count], 0, sizeof(vma_t));
    merges++;
    /* Don't advance i: a may now be mergeable with what followed b. */
  }

  return merges;
}

int64_t vma_unmap_range(vma_set_t *set, uint64_t start, uint64_t end) {
  if (!set || start >= end) {
    return -1;
  }

  /* Cut at both boundaries first so every affected VMA is fully inside the
   * range and releasing whole VMAs can never strand pages. */
  if (!vma_split_at(set, start) || !vma_split_at(set, end)) {
    return -1;
  }

  int64_t released = 0;
  uint8_t i = 0;
  while (i < set->count) {
    vma_t *v = &set->entries[i];
    if (v->start >= start && v->end <= end) {
      uint64_t n = vma_npages(v);
      for (uint64_t p = 0; p < n; p++) {
        if (v->pages[p]) {
          pmm_free_page(v->pages[p]);
          released++;
        }
      }
      kfree(v->pages);

      for (uint8_t j = i; j + 1 < set->count; j++) {
        set->entries[j] = set->entries[j + 1];
      }
      set->count--;
      memset(&set->entries[set->count], 0, sizeof(vma_t));
      continue; /* don't advance; the shifted entry needs checking too */
    }
    i++;
  }

  /* Weld the neighbours left behind, so repeated munmap/mmap cycles don't
   * burn a VMA slot each time. */
  vma_merge_adjacent(set);
  return released;
}

int vma_protect_range(vma_set_t *set, uint64_t start, uint64_t end,
                      uint32_t prot) {
  if (!set || start >= end) {
    return -1;
  }

  if (!vma_split_at(set, start) || !vma_split_at(set, end)) {
    return -1;
  }

  int touched = 0;
  for (uint8_t i = 0; i < set->count; i++) {
    vma_t *v = &set->entries[i];
    if (v->start >= start && v->end <= end) {
      v->prot = prot;
      touched++;
    }
  }

  /* Neighbours can become identical after a re-protect (e.g. PROT_NONE on
   * one half of a split region), so re-weld. */
  vma_merge_adjacent(set);
  return touched;
}

uint64_t vma_resident_pages(const vma_set_t *set) {
  if (!set) {
    return 0;
  }
  uint64_t total = 0;
  for (uint8_t i = 0; i < set->count; i++) {
    const vma_t *v = &set->entries[i];
    uint64_t n = vma_npages(v);
    for (uint64_t p = 0; p < n; p++) {
      if (v->pages[p]) {
        total++;
      }
    }
  }
  return total;
}

uint64_t vma_mapped_pages(const vma_set_t *set) {
  if (!set) {
    return 0;
  }
  uint64_t total = 0;
  for (uint8_t i = 0; i < set->count; i++) {
    total += vma_npages(&set->entries[i]);
  }
  return total;
}

void vma_release_pages(vma_t *vma) {
  if (!vma || !vma->pages) {
    return;
  }
  uint64_t n = vma_npages(vma);
  for (uint64_t i = 0; i < n; i++) {
    if (vma->pages[i]) {
      pmm_free_page(vma->pages[i]);
      vma->pages[i] = 0;
    }
  }
  kfree(vma->pages);
  vma->pages = 0;
}

void vma_destroy(vma_set_t *set) {
  if (!set) {
    return;
  }
  for (uint8_t i = 0; i < set->count; i++) {
    vma_release_pages(&set->entries[i]);
  }
  set->count = 0;
}