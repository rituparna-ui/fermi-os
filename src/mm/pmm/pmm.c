#include "pmm.h"
#include "mm/vm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"
#include <stdint.h>

/*
 * __kernel_end is a *virtual* address (set by the linker in the
 * higher-half region).  We need its physical counterpart to know
 * where we can start placing the bitmap in physical RAM.
 */
extern uintptr_t __kernel_end;

static uint64_t *bitmap;       /* virtual address of the bitmap */
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

  uart_puts("[PMM][INFO] Memory region: ");
  uart_puthex(mem_region_start);
  uart_puts(" - ");
  uart_puthex(mem_region_end);
  uart_println("");

  uart_puts("[PMM][INFO] Memory Size: ");
  uart_puthex(mem_size);
  uart_puts(" | ");
  uart_putdec(mem_size / 1024 / 1024);
  uart_println(" mbytes");

  uart_puts("[PMM][INFO] Total Pages: ");
  uart_putdec(total_pages);
  uart_println("");

  uart_puts("[PMM][INFO] Reserved Pages: ");
  uart_putdec(reserved_pages);
  uart_println("");

  uart_puts("[PMM][INFO] Used Pages: ");
  uart_putdec(used_pages);
  uart_println("");

  uart_puts("[PMM][INFO] Free Pages: ");
  uart_putdec(total_pages - used_pages);
  uart_println("");
}

void pmm_init(uintptr_t mem_start, uint64_t mem_size) {
  uart_println("[PMM] Initializing Physical Memory Manager");
  mem_region_start = mem_start;
  mem_region_end = mem_start + mem_size;

  total_pages = mem_size / PAGE_SIZE;

  /* one bit per page — need ceil(total_pages / 64) uint64_t entries */
  bitmap_size = (total_pages + 63) / 64;
  uint64_t bitmap_bytes = bitmap_size * sizeof(uint64_t);

  /*
   * __kernel_end is a virtual address.  Convert to physical so we
   * can place the bitmap right after the kernel image in physical
   * RAM, then access it through its virtual (higher-half) address.
   */
  uint64_t kernel_end_virt = (uint64_t)&__kernel_end;
  uint64_t kernel_end_phys = VIRT_TO_PHYS(kernel_end_virt);
  uint64_t bitmap_phys = PAGE_ALIGN_UP(kernel_end_phys);

  /* We access the bitmap via its virtual address */
  bitmap = (uint64_t *)PHYS_TO_VIRT(bitmap_phys);

  uart_puts("[PMM] Bitmap at VA ");
  uart_puthex((uint64_t)bitmap);
  uart_puts(" (PA ");
  uart_puthex(bitmap_phys);
  uart_println(")");

  uart_println("[PMM] Zeroing Bitmap");
  memset(bitmap, 0, bitmap_bytes);
  uart_println("[PMM] Mark kernel and bitmap space reserved");

  /* kernel image + stack + bitmap — all in physical space */
  uint64_t bitmap_end_phys = bitmap_phys + bitmap_bytes;
  uint64_t reserved_end = PAGE_ALIGN_UP(bitmap_end_phys);
  reserved_pages = (reserved_end - mem_region_start) / PAGE_SIZE;

  /* mark reserved pages */
  for (uint64_t pfn = 0; pfn < reserved_pages; pfn++) {
    bitmap_set(pfn);
  }

  used_pages = reserved_pages;
  uart_println("[PMM] Initialized!");
}

/*
 * pmm_allocate_page — returns a **physical** address.
 * The caller is responsible for converting to virtual if needed.
 */
uintptr_t pmm_allocate_page(void) {
  for (uint64_t i = 0; i < bitmap_size; i++) {
    if (bitmap[i] == ~0ULL) {
      continue;
    }

    for (uint8_t bit = 0; bit < 64; bit++) {
      uint64_t page_frame_number = i * 64 + bit;
      if (page_frame_number >= total_pages) {
        uart_errorln("[PMM] Out of range pfn.");
        return 0;
      }

      if (!bitmap_test(page_frame_number)) {
        bitmap_set(page_frame_number);
        used_pages++;
        uintptr_t phys_addr = mem_region_start + PFN_TO_PHYS(page_frame_number);
        return phys_addr;
      }
    }
  }

  uart_errorln("[PMM] Out of memory! No free pages available.");
  return 0;
}

void pmm_free_page(uintptr_t phys_addr) {
  if (phys_addr < mem_region_start || phys_addr >= mem_region_end) {
    uart_errorln("[PMM] address outside managed region");
    return;
  }

  if (phys_addr & (PAGE_SIZE - 1)) {
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
