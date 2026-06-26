#include "hyp_mmu_el2.h"
#include "hyp_alloc.h"
#include "hyp.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EL2 stage-1 identity MMU. See hyp_mmu_el2.h for the why.
 *
 * VA space: 39 bits (T0SZ = 64 - 39 = 25), 4 KiB granule, start level 1. The L1
 * table has 512 entries, each a 1 GiB block. We cover 0 .. 0x280000000 (10 GiB,
 * indices 0..9); the rest are left invalid.
 * ------------------------------------------------------------------------- */

#define EL2_VA_BITS    39
#define EL2_T0SZ       (64 - EL2_VA_BITS)        /* 25 */
#define L1_GIB         (1ULL << 30)              /* 1 GiB per L1 block */
#define L1_ENTRIES     512
#define MAP_TOP        0x280000000ULL            /* 10 GiB: covers RAM + hyp pool */

/* Block descriptor (level 1, 1 GiB): bits[1:0]=01 (block), valid. */
#define DESC_VALID     (1ULL << 0)
#define DESC_BLOCK     (0ULL << 1)               /* 0 = block at L1/L2 */
#define DESC_AF        (1ULL << 10)              /* Access Flag (else access faults) */
/* Shareability SH[9:8]: 0b11 = Inner Shareable (required for inter-PE coherency). */
#define DESC_SH_IS     (3ULL << 8)
#define DESC_SH_NONE   (0ULL << 8)
/* AttrIndx[4:2] selects a MAIR_EL2 attribute. */
#define DESC_ATTR(idx) (((uint64_t)(idx)) << 2)
/* Stage-1 AP[7:6]: at EL2 (a single exception level) we only need RW; AP=00 =
 * RW at EL2. XN (UXN/PXN) — for EL2 the relevant bit is XN at bit[54]. */
#define DESC_XN        (1ULL << 54)

/* MAIR_EL2 attribute indices. */
#define ATTR_IDX_NORMAL 0  /* 0xFF: Normal WB RW-allocate inner+outer */
#define ATTR_IDX_DEVICE 1  /* 0x00: Device-nGnRnE */
#define MAIR_EL2_VALUE  (0xFFULL << (8 * ATTR_IDX_NORMAL)) | \
                        (0x00ULL << (8 * ATTR_IDX_DEVICE))

/* TCR_EL2 (non-VHE, single TTBR0). Fields: T0SZ[5:0], IRGN0[9:8]=WBWA(01),
 * ORGN0[11:10]=WBWA(01), SH0[13:12]=IS(11), TG0[15:14]=4K(00), PS[18:16].
 * RES1 bits[23] and [31] are set on EL2 TCR. PS=5 (48-bit) is safely within the
 * probed 52-bit PARange and covers our 10 GiB map. */
#define TCR_EL2_IRGN0_WBWA (1ULL << 8)
#define TCR_EL2_ORGN0_WBWA (1ULL << 10)
#define TCR_EL2_SH0_IS     (3ULL << 12)
#define TCR_EL2_TG0_4K     (0ULL << 14)
#define TCR_EL2_PS_48      (5ULL << 16)
#define TCR_EL2_RES1       ((1ULL << 23) | (1ULL << 31))
#define TCR_EL2_VALUE      (EL2_T0SZ | TCR_EL2_IRGN0_WBWA | TCR_EL2_ORGN0_WBWA | \
                            TCR_EL2_SH0_IS | TCR_EL2_TG0_4K | TCR_EL2_PS_48 | \
                            TCR_EL2_RES1)

#define SCTLR_EL2_M (1ULL << 0)
#define SCTLR_EL2_C (1ULL << 2)
#define SCTLR_EL2_I (1ULL << 12)

/* The single L1 table (built once by CPU0, shared read-only by all cores). */
static uint64_t *el2_l1;

void hyp_mmu_el2_build(void) {
  /* One 4 KiB page = 512 x 8-byte descriptors. From the boot bump allocator
   * (MMU still off here, so the returned PA == VA). */
  el2_l1 = (uint64_t *)hyp_alloc_pages(1);

  for (uint64_t i = 0; i < L1_ENTRIES; i++) {
    uint64_t va = i * L1_GIB;
    if (va >= MAP_TOP) {
      el2_l1[i] = 0; /* invalid */
      continue;
    }
    uint64_t desc = (va & ~(L1_GIB - 1)) | DESC_VALID | DESC_BLOCK | DESC_AF;
    if (va < L1_GIB) {
      /* GiB 0: all device MMIO on QEMU virt (GIC, UART, virtio, …). */
      desc |= DESC_ATTR(ATTR_IDX_DEVICE) | DESC_SH_NONE | DESC_XN;
    } else {
      /* GiB 1..9: RAM (guest RAM + hyp image/pool). Normal-WB Inner-Shareable,
       * executable (the hyp's own .text lives at 0x250000000). */
      desc |= DESC_ATTR(ATTR_IDX_NORMAL) | DESC_SH_IS;
    }
    el2_l1[i] = desc;
  }

  /* Publish the table to PoC: the table walker reads it as cacheable once the
   * MMU is on, but it was written here with the MMU off (Non-cacheable). */
  hyp_dcache_clean_range((uint64_t)(uintptr_t)el2_l1, L1_ENTRIES * 8);
  __asm__ __volatile__("dsb sy" ::: "memory");

  hyp_puts("[EL2MMU] L1 built @ ");
  hyp_puthex((uint64_t)(uintptr_t)el2_l1);
  hyp_puts(" (GiB0=Device, GiB1-9=Normal-WB-IS)\n");
}

void hyp_mmu_el2_enable(void) {
  uint64_t mair = MAIR_EL2_VALUE;
  uint64_t tcr = TCR_EL2_VALUE;
  uint64_t ttbr = (uint64_t)(uintptr_t)el2_l1;

  __asm__ __volatile__(
      "msr mair_el2, %0\n\t"
      "msr tcr_el2,  %1\n\t"
      "msr ttbr0_el2,%2\n\t"
      "isb\n\t"
      "tlbi alle2\n\t"          /* drop any stale EL2 translations */
      "dsb sy\n\t"
      "isb\n\t"
      ::"r"(mair), "r"(tcr), "r"(ttbr) : "memory");

  /* Turn on M (MMU) + C (data cache) + I (instruction cache). */
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el2" : "=r"(sctlr));
  sctlr |= SCTLR_EL2_M | SCTLR_EL2_C | SCTLR_EL2_I;
  __asm__ __volatile__("msr sctlr_el2, %0\n\tisb" ::"r"(sctlr) : "memory");
}
