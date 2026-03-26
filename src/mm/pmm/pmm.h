#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>

/*
 * QEMU virt board physical memory layout:
 *   PA 0x4000_0000 – 0x23FFF_FFFF  =  8 GB RAM (with -m 8G)
 */
#define MEM_START 0x40000000ULL
#define MEM_SIZE  (8ULL * 1024 * 1024 * 1024)
#define PAGE_SIZE 4096
/* Page size = 2^PAGE_SHIFT */
#define PAGE_SHIFT 12

/* Convert between page frame number and physical address */
#define PFN_TO_PHYS(pfn) ((uint64_t)(pfn) << PAGE_SHIFT)
#define PHYS_TO_PFN(addr) ((uint64_t)(addr) >> PAGE_SHIFT)

/* Align address up/down to page boundary */
#define PAGE_ALIGN_UP(addr)   (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))

/* each uint64_t holds 64 page bits */
#define BITMAP_INDEX(pfn) ((pfn) / 64)
#define BITMAP_BIT(pfn)   ((pfn) % 64)

void pmm_init(uintptr_t mem_start, uint64_t mem_size);
uintptr_t pmm_allocate_page(void);
void pmm_free_page(uintptr_t phys_addr);
void pmm_print_info(void);

#endif
