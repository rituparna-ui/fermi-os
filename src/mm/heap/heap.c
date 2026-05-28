#include "heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

static block_header_t *heap_head = 0;

// Track each PMM-backed region so kfree can sanity-check that a freed
// pointer falls within *some* heap region. Address-adjacent blocks may
// span allocations that came from non-contiguous PMM frames, so coalescing
// must verify adjacency rather than blindly merging neighbours.
#define HEAP_MAX_REGIONS 16
struct heap_region {
  uintptr_t va_start;
  uint64_t size_bytes;
};
static struct heap_region regions[HEAP_MAX_REGIONS];
static uint32_t region_count = 0;

// Grow at least this many pages per heap_expand call to amortize the
// cost of pmm_allocate_pages and bookkeeping.
#define HEAP_EXPAND_MIN_PAGES 64

static int register_region(uintptr_t va, uint64_t bytes) {
  if (region_count >= HEAP_MAX_REGIONS) {
    uart_errorln("[HEAP] region table full");
    return -1;
  }
  regions[region_count].va_start = va;
  regions[region_count].size_bytes = bytes;
  region_count++;
  return 0;
}

static int addr_in_any_region(uintptr_t addr) {
  for (uint32_t i = 0; i < region_count; i++) {
    if (addr >= regions[i].va_start &&
        addr < regions[i].va_start + regions[i].size_bytes) {
      return 1;
    }
  }
  return 0;
}

void heap_init(void) {
  uart_println("[HEAP] Initializing");

  uint64_t pages = HEAP_INITIAL_PAGES;
  uintptr_t phys = pmm_allocate_pages(pages);
  if (!phys) {
    uart_errorln("[HEAP] Failed to allocate pages for heap");
    return;
  }

  uintptr_t va = PHYS_TO_VIRT(phys);
  uint64_t heap_size = pages * PAGE_SIZE;

  memset((void *)va, 0, heap_size);

  // create a single free block spanning the entire heap
  // layout: [block_header_t | usable payload ...]
  heap_head = (block_header_t *)va;
  heap_head->size = heap_size - BLOCK_HEADER_SIZE;
  heap_head->is_free = 1;
  heap_head->magic = BLOCK_MAGIC_FREE;
  heap_head->next = 0;

  register_region(va, heap_size);

  uart_printf("[HEAP] Heap VA: %x - %x\n", va, va + heap_size);
  uart_printf("[HEAP] Usable: %d KiB (%d bytes) | Header: %d bytes\n",
              heap_head->size / 1024, heap_head->size, BLOCK_HEADER_SIZE);

  uart_println("[HEAP] Initialized!");
}

// Try to grow the heap by allocating more PMM pages. Returns 0 on success,
// -1 on failure. The new region is appended to the address-ordered free
// list as a single free block; because PMM may hand back non-adjacent
// physical frames between calls, the coalescing walk in kfree verifies
// physical adjacency before merging across blocks.
static int heap_expand(size_t need_bytes) {
  uint64_t bytes_required = need_bytes + BLOCK_HEADER_SIZE;
  uint64_t pages = (bytes_required + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages < HEAP_EXPAND_MIN_PAGES) {
    pages = HEAP_EXPAND_MIN_PAGES;
  }

  uintptr_t phys = pmm_allocate_pages(pages);
  if (!phys) {
    uart_errorln("[HEAP] expand: pmm_allocate_pages failed");
    return -1;
  }

  uintptr_t va = PHYS_TO_VIRT(phys);
  uint64_t bytes = pages * PAGE_SIZE;
  memset((void *)va, 0, bytes);

  block_header_t *new_block = (block_header_t *)va;
  new_block->size = bytes - BLOCK_HEADER_SIZE;
  new_block->is_free = 1;
  new_block->magic = BLOCK_MAGIC_FREE;
  new_block->next = 0;

  // Append at end of list (kept in insertion order; coalescing handles
  // ordering inconsistencies via the adjacency check.)
  block_header_t *tail = heap_head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = new_block;

  if (register_region(va, bytes) != 0) {
    /* Region table overflow — unlink to avoid an unverifiable region. */
    tail->next = 0;
    pmm_free_pages(phys, pages);
    return -1;
  }

  uart_printf("[HEAP] Expanded by %d KiB (%d pages) at VA %x\n", bytes / 1024,
              pages, va);
  return 0;
}

// first fit algorithm
// traverse the linked list from heap_head looking for the first free block
// whose size >= requested size
void *kmalloc(size_t size) {
  if (size == 0) {
    return 0;
  }

  // Reject sizes that would overflow the 16-byte alignment rounding.
  // HEAP_ALIGN_UP(x) computes (x + 15) & ~15; if x > SIZE_MAX - 15 the
  // addition wraps to a small value, which would let the first-fit search
  // succeed and return a much-smaller-than-requested block. The caller's
  // write would then trash adjacent heap metadata.
  if (size > SIZE_MAX - (HEAP_ALIGN - 1)) {
    uart_errorln("[HEAP] kmalloc: size overflow");
    return 0;
  }

  size = HEAP_ALIGN_UP(size);

  // Try once, expand the heap on failure, try once more.
  for (int attempt = 0; attempt < 2; attempt++) {
    block_header_t *current = heap_head;

    while (current) {
      if (current->is_free && current->size >= size) {
        // found a suitable block
        // split if enough leftover for a new block
        size_t remaining = current->size - size;
        if (remaining > BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE) {
          /*
            split
            BEFORE:  [ header | ========== big free block ========== ]
            AFTER:   [ header | allocated ] [ new_header | remaining free ]
          */
          block_header_t *new_block =
              (block_header_t *)((uint8_t *)current + BLOCK_HEADER_SIZE + size);
          new_block->size = remaining - BLOCK_HEADER_SIZE;
          new_block->is_free = 1;
          new_block->magic = BLOCK_MAGIC_FREE;
          new_block->next = current->next;

          current->size = size;
          current->next = new_block;
        }

        current->is_free = 0;
        current->magic = BLOCK_MAGIC_ALLOC;
        // + BLOCK_HEADER_SIZE to reach forward to the payload address
        return (void *)((uint8_t *)current + BLOCK_HEADER_SIZE);
      }

      current = current->next;
    }

    // No fit — try to grow the heap once.
    if (heap_expand(size) != 0) {
      break;
    }
  }

  uart_errorln("[HEAP] kmalloc: out of memory!");
  return 0;
}

void kfree(void *ptr) {
  if (!ptr) {
    return;
  }

  // - BLOCK_HEADER_SIZE to reach backwards to the header address
  block_header_t *block =
      (block_header_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);

  uintptr_t block_addr = (uintptr_t)block;

  if (!addr_in_any_region(block_addr)) {
    uart_errorln("[HEAP] kfree: pointer outside heap regions!");
    return;
  }

  if (block->magic != BLOCK_MAGIC_ALLOC) {
    /* Either a wild pointer, or the block has already been freed (its magic
     * was rewritten to FREE), or memory next to the header was clobbered.
     * Either way, do not touch the free list. */
    uart_printf("[HEAP] kfree: bad magic %x at %x — refusing\n",
                (uint64_t)block->magic, (uint64_t)block_addr);
    return;
  }


  if (block->is_free) {
    uart_errorln("[HEAP] kfree: double free detected!");
    return;
  }

  block->is_free = 1;
  block->magic = BLOCK_MAGIC_FREE;

  /*
    coalescing
    walk the entire list and merge consecutive free blocks, but only
    when they are *physically* adjacent — heap_expand may have appended
    blocks that are non-contiguous in memory.
    BEFORE:  [ hdr | free 64B ] → [ hdr | free 128B ] → ...
    AFTER:   [ hdr | free 64B + hdr_size + 128B      ] → ...
  */
  block_header_t *current = heap_head;
  while (current) {
    while (current->is_free && current->next && current->next->is_free) {
      uintptr_t end_of_current =
          (uintptr_t)current + BLOCK_HEADER_SIZE + current->size;
      if (end_of_current != (uintptr_t)current->next) {
        break; // gap between blocks — can't safely merge
      }
      current->size += BLOCK_HEADER_SIZE + current->next->size;
      current->next = current->next->next;
    }
    current = current->next;
  }
}

void heap_print_info(void) {
  uart_println("[HEAP][INFO] Heap block list:");

  block_header_t *current = heap_head;
  uint64_t total_free = 0;
  uint64_t total_used = 0;
  uint64_t block_count = 0;

  while (current) {
    uart_printf("  [%d] addr=%x size=%d %s\n", block_count,
                (uint64_t)(uintptr_t)current, current->size,
                current->is_free ? "FREE" : "USED");

    if (current->is_free) {
      total_free += current->size;
    } else {
      total_used += current->size;
    }

    block_count++;
    current = current->next;
  }

  uart_printf("[HEAP][INFO] Blocks: %d | Used: %d bytes | Free: %d bytes "
              "| Regions: %d\n",
              block_count, total_used, total_free, region_count);
}

// Total bytes currently used (allocated payload only, excludes headers).
uint64_t heap_used_bytes(void) {
  uint64_t total = 0;
  for (block_header_t *c = heap_head; c; c = c->next) {
    if (!c->is_free) {
      total += c->size;
    }
  }
  return total;
}

// Total bytes currently in free blocks (excludes headers).
uint64_t heap_free_bytes(void) {
  uint64_t total = 0;
  for (block_header_t *c = heap_head; c; c = c->next) {
    if (c->is_free) {
      total += c->size;
    }
  }
  return total;
}

// Aggregate size of all PMM-backed heap regions in bytes.
uint64_t heap_total_bytes(void) {
  uint64_t total = 0;
  for (uint32_t i = 0; i < region_count; i++) {
    total += regions[i].size_bytes;
  }
  return total;
}

static void test_result(const char *name, int pass) {
  uart_printf("[HEAP TEST] %s: %s\n", name, pass ? "PASS" : "FAIL");
}

void heap_run_tests(void) {
  uart_println("[HEAP TEST] Running heap tests...");

  // Test 1: Basic allocation
  uint64_t *a = (uint64_t *)kmalloc(sizeof(uint64_t));
  test_result("kmalloc returns non-null", a != 0);

  // Test 2: Write and read back
  if (a) {
    *a = 0xDEADBEEF;
    test_result("write/read", *a == 0xDEADBEEF);
  }

  // Test 3: Multiple allocations return different addresses
  uint64_t *b = (uint64_t *)kmalloc(sizeof(uint64_t));
  test_result("different addresses", a != b);

  // Test 4: Larger allocation
  char *buf = (char *)kmalloc(1024);
  test_result("1KB alloc", buf != 0);
  if (buf) {
    memset(buf, 'A', 1024);
    test_result("1KB write/read", buf[0] == 'A' && buf[1023] == 'A');
  }

  // Test 5: Free and reuse
  kfree(a);
  kfree(b);
  uint64_t *c = (uint64_t *)kmalloc(sizeof(uint64_t));
  // after freeing a and b, c should reuse one of those addresses
  test_result("free/reuse", c == a || c == b);

  // Test 6: Free the large buffer and allocate again
  kfree(buf);
  kfree(c);
  char *buf2 = (char *)kmalloc(2048);
  test_result("coalesce + realloc", buf2 != 0);

  // Cleanup
  kfree(buf2);

  heap_print_info();
  uart_println("[HEAP TEST] Done!");
}
