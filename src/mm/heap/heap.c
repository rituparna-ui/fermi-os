#include "heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"

static block_header_t *heap_head = 0;

static uintptr_t heap_phys_base = 0;
static uint64_t heap_page_count = 0;

void heap_init(void) {
  uart_println("[HEAP] Initializing");

  uint64_t pages = HEAP_INITIAL_PAGES;
  uintptr_t phys = pmm_allocate_pages(pages);
  if (!phys) {
    uart_errorln("[HEAP] Failed to allocate pages for heap");
    return;
  }

  heap_phys_base = phys;
  heap_page_count = pages;

  uintptr_t va = PHYS_TO_VIRT(phys);
  uint64_t heap_size = pages * PAGE_SIZE;

  memset((void *)va, 0, heap_size);

  // create a single free block spanning the entire heap
  // layout: [block_header_t | usable payload ...]
  heap_head = (block_header_t *)va;
  heap_head->size = heap_size - BLOCK_HEADER_SIZE;
  heap_head->is_free = 1;
  heap_head->next = 0;

  uart_printf("[HEAP] Heap VA: %x - %x\n", va, va + heap_size);
  uart_printf("[HEAP] Usable: %d KiB (%d bytes) | Header: %d bytes\n",
              heap_head->size / 1024, heap_head->size, BLOCK_HEADER_SIZE);

  uart_println("[HEAP] Initialized!");
}

// first fit algorithm
// traverse the linked list from heap_head looking for the first free block
// whose size >= requested size
void *kmalloc(size_t size) {
  if (size == 0) {
    return 0;
  }

  size = HEAP_ALIGN_UP(size);

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
        new_block->next = current->next;

        current->size = size;
        current->next = new_block;
      }

      current->is_free = 0;
      // + BLOCK_HEADER_SIZE to reach forward to the payload address
      return (void *)((uint8_t *)current + BLOCK_HEADER_SIZE);
    }

    current = current->next;
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
  uintptr_t heap_va_start = PHYS_TO_VIRT(heap_phys_base);
  uintptr_t heap_va_end = heap_va_start + (heap_page_count * PAGE_SIZE);

  if (block_addr < heap_va_start || block_addr >= heap_va_end) {
    uart_errorln("[HEAP] kfree: pointer outside heap region!");
    return;
  }

  if (block->is_free) {
    uart_errorln("[HEAP] kfree: double free detected!");
    return;
  }

  block->is_free = 1;

  /*
    coalescing
    walk the entire list and merge consecutive free blocks
    BEFORE:  [ hdr | free 64B ] → [ hdr | free 128B ] → ...
    AFTER:   [ hdr | free 64B + hdr_size + 128B      ] → ...
  */
  block_header_t *current = heap_head;
  while (current) {
    while (current->is_free && current->next && current->next->is_free) {
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

  uart_printf("[HEAP][INFO] Blocks: %d | Used: %d bytes | Free: %d bytes\n",
              block_count, total_used, total_free);
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
