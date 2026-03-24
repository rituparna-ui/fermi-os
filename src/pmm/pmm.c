#include "pmm.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include <stdint.h>

extern uintptr_t __kernel_end;

static uint64_t *bitmap;
static uint64_t bitmap_size;
static uint64_t total_pages;
static uint64_t used_pages;
static uint64_t reserved_pages;

static uint64_t mem_region_start;
static uint64_t mem_region_end;

static inline void bitmap_set(uint64_t pfn) {
  bitmap[BITMAP_INDEX(pfn)] |= (1ULL << BITMAP_BIT(pfn));
}

static inline void bitmap_clear(uint64_t pfn) {
  bitmap[BITMAP_INDEX(pfn)] &= ~(1ULL << BITMAP_BIT(pfn));
}

static inline int bitmap_test(uint64_t pfn) {
  return (bitmap[BITMAP_INDEX(pfn)] >> BITMAP_BIT(pfn)) & 1;
}

void pmm_init(uintptr_t mem_start, uint64_t mem_size) {
  uart_println("[PMM] Initializing Physical Memory Manager");
  mem_region_start = mem_start;
  mem_region_end = mem_start + mem_size;

  total_pages = mem_size / PAGE_SIZE;

  uart_puts("[PMM] Memory Starts: ");
  uart_puthex(mem_region_start);
  uart_println("");

  uart_puts("[PMM] Memory Ends: ");
  uart_puthex(mem_region_end);
  uart_println("");

  uart_puts("[PMM] Memory Size: ");
  uart_puthex(mem_size);
  uart_puts(" | ");
  uart_putdec(mem_size);
  uart_println(" bytes");

  uart_puts("[PMM] Total Pages: ");
  uart_putdec(total_pages);
  uart_println("");

  // one bit per page.
  // need ceil(total_pages / 64) uint64_t entries
  bitmap_size = (total_pages + 63) / 64;
  uint64_t bitmap_bytes = bitmap_size * sizeof(uint64_t);

  uart_puts("[PMM] Bitmap Length: ");
  uart_putdec(bitmap_size);
  uart_println("");

  uart_puts("[PMM] Bitmap Bytes: ");
  uart_putdec(bitmap_bytes);
  uart_println("");

  // Place bitmap at the first page-aligned address after the kernel
  uint64_t kernel_end = (uint64_t)&__kernel_end;
  bitmap = (uint64_t *)PAGE_ALIGN_UP(kernel_end);

  uart_puts("[PMM] Bitmap address: ");
  uart_puthex((uintptr_t)bitmap);
  uart_println("");

  uart_puts("[PMM] Kernel End: ");
  uart_puthex(kernel_end);
  uart_println("");

  uart_println("[PMM] Zeroing Bitmap");
  memset(bitmap, 0, bitmap_bytes);
  uart_println("[PMM] Mark kernel and bitmap space reserved");

  // kernel image + stack + bitmap
  uint64_t bitmap_end = (uint64_t)bitmap + bitmap_bytes;
  uint64_t reserved_end = PAGE_ALIGN_UP(bitmap_end);
  reserved_pages = (reserved_end - mem_region_start) / PAGE_SIZE;

  uart_puts("[PMM] Bitmap End: ");
  uart_puthex(reserved_end);
  uart_println("");

  uart_puts("[PMM] Reserved Pages: ");
  uart_putdec(reserved_pages);
  uart_println("");

  // mark reserverd pages
  for (uint64_t pfn = 0; pfn < reserved_pages; pfn++) {
    bitmap_set(pfn);
  }

  used_pages = reserved_pages;
  uart_println("[PMM] Initialized!");
}
