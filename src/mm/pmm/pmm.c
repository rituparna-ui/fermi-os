#include "pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
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

void pmm_print_info() {
  uint64_t mem_size = mem_region_end - mem_region_start;

  uart_printf("[PMM][INFO] Memory region: %x - %x\n", mem_region_start,
              mem_region_end);

  uart_printf("[PMM][INFO] Memory Size: %x | %d mbytes\n", mem_size,
              mem_size / 1024 / 1024);

  uart_printf("[PMM][INFO] Total Pages: %d\n", total_pages);
  uart_printf("[PMM][INFO] Reserved Pages: %d\n", reserved_pages);
  uart_printf("[PMM][INFO] Used Pages: %d\n", used_pages);
  uart_printf("[PMM][INFO] Free Pages: %d\n", total_pages - used_pages);
}

void pmm_init(uintptr_t mem_start, uint64_t mem_size) {
  uart_println("[PMM] Initializing Physical Memory Manager");
  mem_region_start = mem_start;
  mem_region_end = mem_start + mem_size;

  total_pages = mem_size / PAGE_SIZE;

  // one bit per page.
  // need ceil(total_pages / 64) uint64_t entries
  bitmap_size = (total_pages + 63) / 64;
  uint64_t bitmap_bytes = bitmap_size * sizeof(uint64_t);

  uart_printf("[PMM] Bitmap Length: %d\n", bitmap_size);
  uart_printf("[PMM] Bitmap Bytes: %d\n", bitmap_bytes);

  // Place bitmap at the first page-aligned address after the kernel
  uint64_t kernel_end = (uint64_t)&__kernel_end;
  // assert(bitmap + bitmap_bytes < mem_region_end);
  bitmap = (uint64_t *)PAGE_ALIGN_UP(kernel_end);

  uart_printf("[PMM] Bitmap address: %x\n", (uintptr_t)bitmap);
  uart_printf("[PMM] Kernel End: %x\n", kernel_end);

  uart_println("[PMM] Zeroing Bitmap");
  memset(bitmap, 0, bitmap_bytes);
  uart_println("[PMM] Mark kernel and bitmap space reserved");

  // kernel image + stack + bitmap
  uint64_t bitmap_end = (uint64_t)bitmap + bitmap_bytes;
  uint64_t reserved_end = PAGE_ALIGN_UP(bitmap_end);
  reserved_pages = (reserved_end - mem_region_start) / PAGE_SIZE;

  uart_printf("[PMM] Bitmap End: %x\n", reserved_end);

  // mark reserverd pages
  for (uint64_t pfn = 0; pfn < reserved_pages; pfn++) {
    bitmap_set(pfn);
  }

  used_pages = reserved_pages;
  uart_println("[PMM] Initialized!");
}

uintptr_t pmm_allocate_page(void) {
  // uart_printf("[PMM] allocating 1 page at ");

  for (uint64_t i = 0; i < bitmap_size; i++) {
    if (bitmap[i] == ~0ULL) {
      // skip all ones
      continue;
    }

    // find first unset bit in current uint64_t entry
    for (uint8_t bit = 0; bit < 64; bit++) {
      uint64_t page_frame_number = i * 64 + bit;
      if (page_frame_number >= total_pages) {
        uart_errorln("[PMM] Out of range pfm.");
        return 0;
      }

      if (!bitmap_test(page_frame_number)) {
        bitmap_set(page_frame_number);
        used_pages++;
        uintptr_t phys_addr = mem_region_start + PFN_TO_PHYS(page_frame_number);
        // uart_printf("%x\n",phys_addr);
        return phys_addr;
      }
    }
  }

  uart_errorln("[PMM] Out of memory! No free pages available.");
  return 0;
}

uintptr_t pmm_allocate_pages(uint64_t count) {
  if (count == 0) {
    return 0;
  }
  if (count == 1) {
    return pmm_allocate_page();
  }

  uart_printf("[PMM] allocating %d contiguous pages... ", count);

  uint64_t run_start = 0;
  uint64_t run_length = 0;

  for (uint64_t pfn = reserved_pages; pfn < total_pages; pfn++) {
    if (!bitmap_test(pfn)) {
      if (run_length == 0) {
        run_start = pfn;
      }
      run_length++;

      if (run_length == count) {
        for (uint64_t i = 0; i < count; i++) {
          bitmap_set(run_start + i);
        }
        used_pages += count;
        uintptr_t phys_addr = mem_region_start + PFN_TO_PHYS(run_start);
        uart_printf("at %x\n", phys_addr);
        return phys_addr;
      }
    } else {
      run_length = 0;
    }
  }

  uart_errorln("[PMM] No contiguous block found!");
  return 0;
}

void pmm_free_pages(uintptr_t phys_addr, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    pmm_free_page(phys_addr + (i * PAGE_SIZE));
  }
}

void pmm_free_page(uintptr_t phys_addr) {
  uart_printf("[PMM] attempting to free address %x\n", phys_addr);

  if (phys_addr < mem_region_start || phys_addr >= mem_region_end) {
    uart_errorln("[PMM] address outside managed region");
    return;
  }

  if (phys_addr & (PAGE_SIZE - 1)) {
    // lower 12 bits non zero
    uart_errorln("[PMM] non page aligned address");
    return;
  }

  uint64_t page_frame_number = PHYS_TO_PFN(phys_addr - mem_region_start);

  if (page_frame_number < reserved_pages) {
    uart_errorln("[PMM] reserved page");
    return;
  }

  if (!bitmap_test(page_frame_number)) {
    uart_errorln("[PMM] unallocated page");
    return;
  }

  bitmap_clear(page_frame_number);
  used_pages--;
}