#include "hyp_alloc.h"
#include "hyp.h"

/* End of the hypervisor image (text+rodata+data+bss+stack), from linker_hyp.ld.
 * Everything from here to HYP_RAM_TOP is the hyp's private page pool. */
extern uint8_t __hyp_end[];

static uint64_t bump_next; /* next free physical address (== VA, MMU off) */
static int      bump_init_done;

static inline uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + (a - 1)) & ~(a - 1);
}

static void bump_init(void) {
  bump_next = align_up((uint64_t)(uintptr_t)__hyp_end, HYP_PAGE_SIZE);
  bump_init_done = 1;
}

static void zero_pages(void *base, uint64_t pages) {
  /* MMU off → plain stores. Word-at-a-time is fine for 4 KiB-multiples. */
  volatile uint64_t *p = (volatile uint64_t *)base;
  uint64_t words = (pages * HYP_PAGE_SIZE) / sizeof(uint64_t);
  for (uint64_t i = 0; i < words; i++) {
    p[i] = 0;
  }
}

void *hyp_alloc_aligned(uint64_t pages, uint64_t align) {
  if (!bump_init_done) {
    bump_init();
  }
  if (align < HYP_PAGE_SIZE) {
    align = HYP_PAGE_SIZE;
  }
  uint64_t base = align_up(bump_next, align);
  uint64_t end = base + pages * HYP_PAGE_SIZE;
  if (end > HYP_RAM_TOP) {
    hyp_panic("hyp_alloc: out of hypervisor page pool");
  }
  bump_next = end;
  zero_pages((void *)(uintptr_t)base, pages);
  return (void *)(uintptr_t)base;
}

void *hyp_alloc_pages(uint64_t pages) {
  return hyp_alloc_aligned(pages, HYP_PAGE_SIZE);
}

void hyp_dcache_clean_range(uint64_t start, uint64_t len) {
  /* Clean by VA to PoC over the range, one cache line at a time. CTR_EL0.DminLine
   * gives the smallest data-cache line size as log2(words); use a conservative
   * 64-byte stride which is a multiple-or-divisor-safe choice on QEMU. */
  uint64_t line = 64;
  uint64_t addr = start & ~(line - 1);
  uint64_t end = start + len;
  for (; addr < end; addr += line) {
    __asm__ __volatile__("dc cvac, %0" ::"r"(addr) : "memory");
  }
  __asm__ __volatile__("dsb ish" ::: "memory");
}

void hyp_dcache_inval_range(uint64_t start, uint64_t len) {
  /* Clean+invalidate by VA to PoC ('dc civac') over the range. Used before EL2
   * (MMU off => Normal Non-cacheable) READS memory the guest wrote as Normal-WB
   * cacheable — without this, EL2 would not snoop the guest's dirty cache lines
   * and could read stale data (e.g. a virtio avail ring / descriptor table).
   * civac (not plain ivac) so a line EL2 itself dirtied is flushed first, never
   * discarded. Same 64-byte stride as the clean helper. */
  uint64_t line = 64;
  uint64_t addr = start & ~(line - 1);
  uint64_t end = start + len;
  for (; addr < end; addr += line) {
    __asm__ __volatile__("dc civac, %0" ::"r"(addr) : "memory");
  }
  __asm__ __volatile__("dsb ish" ::: "memory");
}
