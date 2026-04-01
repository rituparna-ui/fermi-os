#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define MEM_START 0x40000000ULL
#define MEM_SIZE (8ULL * 1024 * 1024 * 1024)
#define PAGE_SIZE 4096
// Page size = 2^PAGE_SHIFT
#define PAGE_SHIFT 12

// Convert between page frame number and physical address
// phys_addr = pfn * PAGE_SIZE
// x << 12 == x * 4096
// works for page aligned addresses
#define PFN_TO_PHYS(pfn) ((uint64_t)(pfn) << PAGE_SHIFT)
#define PHYS_TO_PFN(addr) ((uint64_t)(addr) >> PAGE_SHIFT)

// Align address up/down to page boundary
// clear lower 12 bits
// addr = 0x12345 | up = 0x13000 | down = 0x12000
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))

// each uint64_t holds 64 page bits
#define BITMAP_INDEX(pfn) ((pfn) / 64)
#define BITMAP_BIT(pfn) ((pfn) % 64)

void pmm_init(uintptr_t mem_start, uint64_t mem_size);
uintptr_t pmm_allocate_page(void);
uintptr_t pmm_allocate_pages(uint64_t count);
void pmm_free_page(uintptr_t phys_addr);
void pmm_free_pages(uintptr_t phys_addr, uint64_t count);
void pmm_print_info(void);

#endif