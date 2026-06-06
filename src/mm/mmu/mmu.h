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
// Access Permissions (AP[7:6] in block/page descriptor)
// Stage 1 EL1&0 translation regime:
//   AP[2:1]  EL1    EL0
//   00       RW     None
//   01       RW     RW
//   10       RO     None
//   11       RO     RO
#define PTE_AP_RW (0ULL << 6)     // EL1 RW, EL0 no access
#define PTE_AP_RW_EL0 (1ULL << 6) // EL1 RW, EL0 RW
#define PTE_AP_RO (2ULL << 6)     // EL1 RO, EL0 no access
#define PTE_AP_RO_EL0 (3ULL << 6) // EL1 RO, EL0 RO
// memory type from MAIR_EL1
#define PTE_ATTRIDX(idx) ((idx) << 2)

#define _512GB 0x8000000000ULL
#define _1GB 0x40000000ULL
#define _2MB 0x200000ULL

#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL
#define PHYS_TO_VIRT(pa) ((pa) + KERNEL_VA_OFFSET)
#define VIRT_TO_PHYS(va) ((va) - KERNEL_VA_OFFSET)

#define L0_INDEX(va) (((va) >> 39) & 0x1FF)
#define L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define L2_INDEX(va) (((va) >> 21) & 0x1FF)
#define L3_INDEX(va) (((va) >> 12) & 0x1FF)

// User cannot execute
#define PTE_UXN (1ULL << 54)
// kernel cannot execute
#define PTE_PXN (1ULL << 53)
// non-Global: TLB entries from this PTE are tagged with the current ASID
// (TTBR0_EL1[63:48]). Kernel mappings leave nG=0 so they are global and
// shared across every ASID; user mappings set nG=1 so an ASID change in
// TTBR0_EL1 makes the previous task's user TLB entries inaccessible without
// a flush.
#define PTE_NG (1ULL << 11)

// TTBR_EL1 layout when TCR_EL1.AS=1 (16-bit ASIDs) and TCR_EL1.A1=0:
//   bits[63:48] = ASID
//   bits[47:1]  = page-table base address (page-aligned, so [11:0] = 0)
//   bit [0]     = CnP (we leave 0)
#define TTBR_ASID_SHIFT 48
#define TTBR_BADDR_MASK 0x0000FFFFFFFFFFFFULL

static inline uint64_t ttbr_pack(uint64_t baddr, uint16_t asid) {
  return (baddr & TTBR_BADDR_MASK) | ((uint64_t)asid << TTBR_ASID_SHIFT);
}
static inline uint64_t ttbr_baddr(uint64_t ttbr) {
  return ttbr & TTBR_BADDR_MASK;
}
static inline uint16_t ttbr_asid(uint64_t ttbr) {
  return (uint16_t)(ttbr >> TTBR_ASID_SHIFT);
}



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

// User-space address layout (TTBR0)
#define USER_TEXT_BASE 0x00400000ULL // 4 MB — user code
#define USER_STACK_TOP 0x00800000ULL // 8 MB — top of user stack
#define USER_STACK_PAGES 4           // 16 KiB user stack

uint64_t *mmu_init(void);
// empty TTBR0 page table for user task
uint64_t *mmu_create_user_tables(void);

// Map a contiguous run of 4 KiB pages [pa, pa + pages*PAGE_SIZE) into the
// user page table at virtual address va.
// Preconditions: va and pa are 4 KiB-aligned. flags carry the AP / UXN /
// PXN / ATTRIDX bits; PTE_VALID, PTE_AF and PTE_SH_INNER are added
// internally.
void mmu_map_user_range(uint64_t *l0, uint64_t va, uint64_t pa,
                        uint64_t pages, uint64_t flags);

// Free all page table pages (L0, L1, L2, L3) for a user address space
void mmu_free_user_tables(uint64_t *l0);

void mmu_run_tests(uint64_t *l1_table);

#endif
