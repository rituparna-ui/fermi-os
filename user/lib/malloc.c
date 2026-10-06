#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "sys.h"

/*
 * Fermi libc — heap allocator.
 *
 * A first-fit free-list allocator over a single contiguous brk() arena.
 *
 * Design notes
 * ------------
 * The kernel's brk() hands back one contiguous, demand-paged region that grows
 * in 16 MiB steps and commits no physical memory until touched. That makes a
 * classic in-place (non-segregated) allocator viable: growing the arena is a
 * single brk() call, and the only cost of reserving address space is VA.
 *
 * Blocks form a doubly-linked list in ADDRESS order, not a free list. A free
 * list would be faster, but coalescing with the previous block requires
 * back-pointers anyway, and at this scale the linear walk over a page-resident
 * list is far cheaper than the extra bookkeeping.
 *
 * malloc() ZEROES its payload. That's stricter than POSIX, which permits
 * indeterminate bytes. The reason: an allocation can be served from recycled
 * (already-freed) heap, so "fresh from brk" and "recycled" are indistinguishable
 * at this layer, and zeroing removes any chance of leaking one allocation's
 * stale contents into another. Once the allocator tracks which bytes came
 * straight from brk this can be dropped for a real speedup — correctness first.
 *
 * All payload pointers are 16-byte aligned, and sizeof(block_t) is exactly
 * 32, so payload starts at header+32 which stays 16-aligned for any aligned
 * header.
 */

/* Diagnostics go to fd 2. Note this is sys_write, NOT the kernel's uart_*:
 * user space has no access to kernel functions — the kernel .text is mapped
 * read-only for a task's own code, not as a callable library. */
static void diag(const char *s) {
  size_t n = 0;
  while (s[n]) {
    n++;
  }
  sys_write(2, s, n);
}

#define ALIGN      16
#define ALIGN_UP(x) (((x) + (ALIGN - 1)) & ~(size_t)(ALIGN - 1))

#define MAGIC_ALLOC 0xA110CEDUL
#define MAGIC_FREE  0xFEEDF1EEUL

/* Smallest payload worth splitting off; below this a remainder is wasted
 * rather than turned into an unusable block. */
#define MIN_PAYLOAD 16

typedef struct block {
  size_t        size;   /* usable payload bytes (excludes this header) */
  struct block *next;   /* next block in address order */
  struct block *prev;   /* previous block in address order */
  uint32_t      magic;
  uint32_t      is_free;
} block_t;

/* Payload is header + ALIGN_UP(size) bytes. If the header weren't a multiple
 * of ALIGN, a 16-aligned header would still produce a misaligned payload and
 * every 16-byte-requiring allocation would be silently unaligned. */
_Static_assert(sizeof(block_t) % ALIGN == 0,
               "block header must keep payloads aligned");
_Static_assert(sizeof(block_t) <= MIN_PAYLOAD + sizeof(block_t),
               "block header sanity");

static block_t *heap_head;
static block_t *heap_tail;
static char    *heap_brk;   /* current program break */
static int      heap_ready;
static size_t   heap_bytes; /* total bytes requested from the kernel */

#define PAYLOAD(b) ((void *)((char *)(b) + sizeof(block_t)))
#define BLOCK_OF(p) ((block_t *)((char *)(p) - sizeof(block_t)))

/* Ask the kernel for `n` more bytes of arena. Returns the old break on
 * success, NULL if the kernel refused (brk returns the CURRENT break on
 * failure, so comparing against what we asked for is the only way to tell). */
static void *heap_grow(size_t n) {
  if (!heap_ready) {
    heap_brk  = (char *)sys_brk(0);
    heap_ready = 1;
    if (!heap_brk) {
      return NULL;
    }
  }

  char *want = heap_brk + n;
  char *got  = (char *)sys_brk(want);
  if (got != want) {
    return NULL;
  }

  void *old = heap_brk;
  heap_brk = got;
  heap_bytes += n;
  return old;
}

static void heap_append(block_t *b) {
  b->next = NULL;
  b->prev = heap_tail;
  if (heap_tail) {
    heap_tail->next = b;
  } else {
    heap_head = b;
  }
  heap_tail = b;
}

/* Mark free, then merge with whichever neighbours are free. Returns the
 * resulting block (which may be a different pointer than `b`). */
static block_t *block_release(block_t *b) {
  b->is_free = 1;
  b->magic   = MAGIC_FREE;

  /* Merge forward first: doing it last would leave prev->next stale. */
  if (b->next && b->next->is_free) {
    block_t *n = b->next;
    b->size += sizeof(block_t) + n->size;
    b->next  = n->next;
    if (n->next) {
      n->next->prev = b;
    } else {
      heap_tail = b;
    }
    n->magic = 0; /* poison so a later double-free is detected */
  }

  if (b->prev && b->prev->is_free) {
    block_t *p = b->prev;
    p->size += sizeof(block_t) + b->size;
    p->next  = b->next;
    if (b->next) {
      b->next->prev = p;
    } else {
      heap_tail = p;
    }
    b->magic = 0;
    return p;
  }

  return b;
}

void *malloc(size_t n) {
  if (n == 0) {
    n = 1; /* POSIX permits NULL for malloc(0); returning a unique free-able
            * pointer is strictly more useful. */
  }
  size_t need = ALIGN_UP(n);

  /* First fit over the whole list, so coalescing actually keeps the arena
   * compact instead of fragmenting into unusable holes. */
  for (block_t *b = heap_head; b; b = b->next) {
    if (!b->is_free || b->size < need) {
      continue;
    }

    /* Split only when the remainder can hold a header plus a usable payload;
     * otherwise give the whole block over and waste the tail. */
    if (b->size >= need + sizeof(block_t) + MIN_PAYLOAD) {
      block_t *split =
          (block_t *)((char *)PAYLOAD(b) + need);
      split->size    = b->size - need - sizeof(block_t);
      split->is_free = 1;
      split->magic   = MAGIC_FREE;
      split->next    = b->next;
      split->prev    = b;
      if (b->next) {
        b->next->prev = split;
      } else {
        heap_tail = split;
      }
      b->next = split;
      b->size = need;
    }

    b->is_free = 0;
    b->magic   = MAGIC_ALLOC;
    memset(PAYLOAD(b), 0, b->size);
    return PAYLOAD(b);
  }

  /* Nothing fit: extend the arena. Ask for one contiguous chunk holding the
   * new block's header + payload. */
  size_t chunk = need + sizeof(block_t);
  block_t *nb = (block_t *)heap_grow(chunk);
  if (!nb) {
    errno = ENOMEM;
    return NULL;
  }

  nb->size    = need;
  nb->is_free = 0;
  nb->magic   = MAGIC_ALLOC;
  heap_append(nb);

  memset(PAYLOAD(nb), 0, need);
  return PAYLOAD(nb);
}

void free(void *p) {
  if (!p) {
    return; /* free(NULL) is a documented no-op */
  }

  block_t *b = BLOCK_OF(p);
  if (b->magic != MAGIC_ALLOC) {
    /* Not a live block: double free, or a pointer that never came from
       malloc. Refusing is the only safe response — corrupting the free list
       would hand out overlapping memory far more subtly. */
    diag("free(): invalid pointer or double free\n");
    return;
  }

  block_release(b);
}

void *realloc(void *p, size_t n) {
  if (!p) {
    return malloc(n);
  }
  if (n == 0) {
    free(p);
    return NULL;
  }

  block_t *b = BLOCK_OF(p);

  /* Shrinking, or growing within the existing block: keep the same pointer so
   * callers holding interior references stay valid. */
  if (ALIGN_UP(n) <= b->size) {
    size_t keep = ALIGN_UP(n);
    /* If the leftover is big enough to become its own block, split it off and
     * return it to the pool instead of stranding it inside this block. */
    if (b->size >= keep + sizeof(block_t) + MIN_PAYLOAD) {
      block_t *split = (block_t *)((char *)PAYLOAD(b) + keep);
      split->size    = b->size - keep - sizeof(block_t);
      split->is_free = 1;
      split->magic   = MAGIC_FREE;
      split->next    = b->next;
      split->prev    = b;
      if (b->next) {
        b->next->prev = split;
      } else {
        heap_tail = split;
      }
      b->next = split;
      b->size = keep;
      /* New tail is free; merge it if its new neighbour is free too. */
      block_release(split);
    }
    /* Deliberately NOT zeroed here. realloc must preserve the first `keep`
     * bytes; zeroing the payload destroys exactly the data the caller asked to
     * keep. Fresh bytes are zeroed by malloc() instead. */
    return p;
  }

  /* Growing past the block: allocate, copy the live bytes, release the old
   * one. Copy min(old, new) — reading past the old payload would fault. */
  size_t old = b->size;
  void  *np = malloc(n);
  if (!np) {
    return NULL; /* errno already set by malloc; original block still valid */
  }
  memcpy(np, p, old < n ? old : n);
  free(p);
  return np;
}

void *calloc(size_t nmemb, size_t size) {
  /* Guard the multiplication: size_t is 64-bit but a caller can still pass
   * values whose product wraps, which would silently allocate far too little.
   * malloc() already zeroes, so only the overflow check is needed here. */
  if (nmemb != 0 && size > (size_t)-1 / nmemb) {
    errno = ENOMEM;
    return NULL;
  }
  return malloc(nmemb * size);
}

size_t malloc_usable_size(void *p) {
  if (!p) {
    return 0;
  }
  block_t *b = BLOCK_OF(p);
  return b->magic == MAGIC_ALLOC ? b->size : 0;
}

size_t malloc_heap_bytes(void) { return heap_bytes; }

size_t malloc_live_blocks(void) {
  size_t n = 0;
  for (block_t *b = heap_head; b; b = b->next) {
    if (!b->is_free) {
      n++;
    }
  }
  return n;
}

int abs(int n) { return n < 0 ? -n : n; }

long labs(long n) { return n < 0 ? -n : n; }

void qsort(void *base, size_t count, size_t size,
           int (*cmp)(const void *, const void *)) {
  /* Insertion sort: stable, non-recursive, and at the element counts a shell
   * will realistically allocate it is competitive with anything fancier while
   * being far easier to verify correct.
   *
   * Adjacent elements are swapped byte-wise rather than through a temp buffer,
   * so there is no cap on element size — an earlier version stashed elements in
   * a fixed 64-byte buffer and would have silently left oversized arrays
   * unsorted. */
  if (count < 2 || size == 0) {
    return;
  }

  unsigned char *a = (unsigned char *)base;

  for (size_t i = 1; i < count; i++) {
    for (size_t j = i; j > 0; j--) {
      unsigned char *cur  = a + j * size;
      unsigned char *prev = a + (j - 1) * size;
      if (cmp(prev, cur) <= 0) {
        break;
      }
      for (size_t k = 0; k < size; k++) {
        unsigned char t = prev[k];
        prev[k] = cur[k];
        cur[k] = t;
      }
    }
  }
}

void abort(void) {
  diag("abort()\n");
  sys_exit();
}

void exit(int status) {
  (void)status; /* no atexit handlers yet */
  sys_exit();
}