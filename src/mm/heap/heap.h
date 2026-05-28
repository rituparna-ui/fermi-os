#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

// 256 * 4KB = 1MB initial heap
#define HEAP_INITIAL_PAGES 256

// Minimum block split threshold
// Don't split a block if the remainder would be smaller than this
#define HEAP_MIN_BLOCK_SIZE 32

// all allocations 16 bytes aligned
#define HEAP_ALIGN 16
#define HEAP_ALIGN_UP(x) (((x) + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1))

/*
<--------------------- BLOCK --------------------->
[block_header_t (metadata) | payload (usable memory)]
←── BLOCK_HEADER_SIZE ──→ ←──── block->size ──────→
*/
/* Sentinel magics in the header to catch double-free, use-after-free, and
 * wild-pointer frees. Validated on every kfree(); a mismatch logs a
 * diagnostic and refuses the operation rather than corrupting the free list.
 * is_free remains for fast scans — magic and is_free agree on every well-
 * formed block. */
#define BLOCK_MAGIC_ALLOC 0xA110CEDUL
#define BLOCK_MAGIC_FREE  0xFEEDF1EEUL

typedef struct block_header {
  size_t size;               // usable payload size (excludes header)
  uint32_t magic;            // BLOCK_MAGIC_ALLOC | BLOCK_MAGIC_FREE
  uint32_t is_free;          // 1 = free, 0 = allocated (mirror of magic)
  struct block_header *next; // next block in list (address order)
} block_header_t;

#define BLOCK_HEADER_SIZE HEAP_ALIGN_UP(sizeof(block_header_t))

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void heap_print_info(void);
void heap_run_tests(void);

// Stats — used by /proc/meminfo and similar.
uint64_t heap_used_bytes(void);
uint64_t heap_free_bytes(void);
uint64_t heap_total_bytes(void);

#endif
