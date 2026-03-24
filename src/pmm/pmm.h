#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define MEM_START 0x40000000ULL
#define MEM_SIZE (8ULL * 1024 * 1024 * 1024)
#define PAGE_SIZE 4096

// Align address up/down to page boundary
// clear lower 12 bits
// addr = 0x12345 | up = 0x13000 | down = 0x12000
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))

void pmm_init(uintptr_t mem_start, uint64_t mem_size);

#endif