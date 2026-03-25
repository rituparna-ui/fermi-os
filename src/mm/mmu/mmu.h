#ifndef MM_MMU_H
#define MM_MMU_H

#include <stdint.h>

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

uint64_t *mmu_init(void);
void mmu_run_tests(uint64_t *l1_table);

#endif