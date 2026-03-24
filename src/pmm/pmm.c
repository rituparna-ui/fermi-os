#include "pmm.h"
#include "uart/uart.h"
#include "strings/strings.h"
#include <stdint.h>

extern uintptr_t __kernel_end;

void pmm_init(uintptr_t mem_start, uint64_t mem_size) {
  uart_println("[PMM] Initializing Physical Memory Manager");
  uint64_t mem_region_start = mem_start;
  uint64_t mem_region_end = mem_start + mem_size;

  uint64_t total_pages = mem_size / PAGE_SIZE;

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
  uint64_t bitmap_length = (total_pages + 63) / 64;
  uint64_t bitmap_size_bytes = bitmap_length * sizeof(uint64_t);

  uart_puts("[PMM] Bitmap Length: ");
  uart_putdec(bitmap_length);
  uart_println("");

  uart_puts("[PMM] Bitmap Bytes: ");
  uart_putdec(bitmap_size_bytes);
  uart_println("");

  // Place bitmap at the first page-aligned address after the kernel
  uint64_t kernel_end = (uint64_t)&__kernel_end;
  uint64_t *bitmap = (uint64_t *)PAGE_ALIGN_UP(kernel_end);

  uart_puts("[PMM] Bitmap address: ");
  uart_puthex((uintptr_t)bitmap);
  uart_println("");

  uart_puts("[PMM] Kernel End: ");
  uart_puthex(kernel_end);
  uart_println("");

  uart_println("[PMM] Zeroing Bitmap");
  memset(bitmap, 0, bitmap_size_bytes);
  uart_println("[PMM] Mark kernel and bitmap space reserved");

  // kernel image + stack + bitmap
  uint64_t bitmap_end = (uint64_t)bitmap + bitmap_size_bytes;
  uint64_t reserved_end = PAGE_ALIGN_UP(bitmap_end);
  uint64_t reserved_pages = (reserved_end - mem_region_start) / PAGE_SIZE;

  uart_puts("[PMM] Bitmap End: ");
  uart_puthex(reserved_end);
  uart_println("");

  uart_puts("[PMM] Reserved Pages: ");
  uart_putdec(reserved_pages);
  uart_println("");

  // mark reserverd pages
}