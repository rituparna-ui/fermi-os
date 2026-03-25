#include "mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include <stdint.h>

void mmu_init() {
  // reference manual - section D8.3
  uart_println("[MMU] Initializing MMU (48 bit VAS, 4kb granule)");
  // VA layout:
  // [47:39] → L0 (9 bits)
  // [38:30] → L1 (9 bits)
  // [29:21] → L2 (9 bits)
  // [20:12] → L3 (9 bits)
  // [11:0]  → offset
  // Each level has 512 entries (2^9)

  uint64_t *l0_table = (uint64_t *)pmm_allocate_page();
  if (!l0_table) {
    uart_errorln("[MMU] Failed to allocate L0 table");
    return;
  }

  uint64_t *l1_table = (uint64_t *)pmm_allocate_page();
  if (!l1_table) {
    uart_errorln("[MMU] Failed to allocate L1 table");
    return;
  }

  memset((void *)l0_table, 0, PAGE_SIZE);
  memset((void *)l1_table, 0, PAGE_SIZE);

  // VA range 0 → 512GB uses this L1 table
  l0_table[0] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;
  // table descriptor - bit[1:0]
  // 00/10 → invalid
  // 01 → block
  // 11 → table/page

  uint64_t mair = (0x00 << 0) | // AttrIdx 0 = Device memory
                  (0xff << 8);  // AttrIdx 1 = Normal memory

  __asm__ __volatile__("msr mair_el1, %0" ::"r"(mair));

  // Identity map L1 blocks - 8 * 1GB blocks
  for (uint64_t i = 0; i < 8; i++) {
    uint64_t phys = i * 0x40000000ULL;
    uint64_t attr;

    if (phys < 0x40000000ULL) {
      attr = 0; // device memory
    } else {
      attr = 1; // normal memory
    }

    l1_table[i] = phys | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER |
                  PTE_AP_RW | PTE_ATTRIDX(attr);
  }

  uint64_t tcr = (16ULL << 0) |    // T0SZ = 48-bit VA
                 (0b00ULL << 6) |  // Inner & Outer Cachability
                 (0b00ULL << 8) |  // Write Back, Write Allocate
                 (0b11ULL << 12) | // SH0 = inner shareable
                 (0b10ULL << 14);  // TG0 = 4KB granule

  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(tcr));

  __asm__ __volatile__("msr ttbr0_el1, %0" ::"r"(l0_table));
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  // invalidate TLBs
  __asm__ __volatile__("tlbi vmalle1");
  __asm__ __volatile__("dsb ish");
  __asm__ __volatile__("isb");

  // mmu enablement
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));

  sctlr |= (1 << 0);  // M = MMU enable
  sctlr |= (1 << 2);  // C = data cache
  sctlr |= (1 << 12); // I = instruction cache

  __asm__ __volatile__("msr sctlr_el1, %0" ::"r"(sctlr));
  __asm__ __volatile__("isb");

  uart_println("[MMU] Enabled");
}