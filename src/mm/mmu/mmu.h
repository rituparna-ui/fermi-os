#ifndef MM_MMU_H
#define MM_MMU_H

#include <stdint.h>

// table descriptor - bit[1:0]
/*
00/10 -> invalid
01 -> block
11 -> table/page
*/
#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_BLOCK (0ULL << 1)

// Access Flag
// CPU will raise an access fault on first use if AF=0
#define PTE_AF (1ULL << 10)
// Shareability - how memory is shared between cores
#define PTE_SH_INNER (3ULL << 8)
// Access Permissions
/*
| AP bits | Meaning |
| ------- | ------- |
| 00      | EL1 RW  |
| 01      | EL1 RO  |
| 10      | EL0 RW  |
| 11      | EL0 RO  |
*/
#define PTE_AP_RW (0ULL << 6)
// memory type from MAIR_EL1
#define PTE_ATTRIDX(idx) ((idx) << 2)

#define _1GB 0x40000000ULL
#define _2MB 0x200000ULL

#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL
#define PHYS_TO_VIRT(pa) ((pa) + KERNEL_VA_OFFSET)
#define VIRT_TO_PHYS(va) ((va) - KERNEL_VA_OFFSET)

#define L0_INDEX(va) (((va) >> 39) & 0x1FF)
#define L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define L2_INDEX(va) (((va) >> 21) & 0x1FF)

// User cannot execute
#define PTE_UXN (1ULL << 54)
// kernel cannot execute
#define PTE_PXN (1ULL << 53)

// 4KB granule 48-bit OA
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL
// static inline function is
// A small function defined in a header that is
// safe to include everywhere
// optimized to avoid function calls
// private to each source file
static inline uint64_t *pte_next_table(uint64_t entry) {
  return (uint64_t *)(entry & PTE_ADDR_MASK);
}

static inline int pte_valid(uint64_t entry) { return entry & PTE_VALID; }

uint64_t *mmu_init(void);
uint64_t *walk_page_table(uint64_t *l0_table, uint64_t va, int alloc);
void mmu_run_tests(uint64_t *l1_table);

#endif