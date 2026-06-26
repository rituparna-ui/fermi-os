#ifndef HYP_LOCK_H
#define HYP_LOCK_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EL2 spinlock.
 *
 * Correctness depends on the EL2 stage-1 MMU (hyp_mmu_el2.c) being ENABLED so
 * that the lock word lives in Normal-WB Inner-Shareable memory. On the MMU-off
 * Normal Non-cacheable memory used before SMP, ldxr/stxr exclusives have no
 * guaranteed global monitor (ARM DDI 0487) — so a lock word MUST be ordinary
 * cacheable RAM (.bss / the hyp page pool), NEVER a Device/MMIO region.
 *
 * Implementation: a load-acquire-exclusive / store-release-exclusive test-and-
 * set, with WFE on contention and SEV on release (the standard low-power spin).
 * Acquire carries Acquire ordering (ldaxr), release carries Release ordering
 * (stlr) — no separate dmb needed for the critical section.
 * ------------------------------------------------------------------------- */

typedef struct {
  volatile uint32_t lock; /* 0 = free, 1 = held */
} hyp_spinlock_t;

#define HYP_SPINLOCK_INIT { 0 }

static inline void hyp_spin_lock(hyp_spinlock_t *l) {
  uint32_t tmp;
  __asm__ __volatile__(
      "   sevl\n"               /* prime the event register so the first wfe falls through */
      "1: wfe\n"
      "2: ldaxr  %w0, [%1]\n"   /* load-acquire exclusive */
      "   cbnz   %w0, 1b\n"     /* held -> wait for an event (SEV from unlock) */
      "   stxr   %w0, %w2, [%1]\n" /* try to take it; %w0 = 0 on success */
      "   cbnz   %w0, 2b\n"     /* lost the exclusive -> retry without sleeping */
      : "=&r"(tmp)
      : "r"(&l->lock), "r"(1u)
      : "memory");
}

static inline int hyp_spin_trylock(hyp_spinlock_t *l) {
  uint32_t cur, fail;
  __asm__ __volatile__(
      "   ldaxr  %w0, [%2]\n"
      "   cbnz   %w0, 1f\n"     /* already held -> fail */
      "   stxr   %w1, %w3, [%2]\n" /* %w1 = 0 on success */
      "   cbnz   %w1, 1f\n"     /* lost exclusive -> treat as fail (caller retries) */
      "   mov    %w1, #0\n"     /* success: fail=0 */
      "   b      2f\n"
      "1: mov    %w1, #1\n"     /* failure */
      "2:\n"
      : "=&r"(cur), "=&r"(fail)
      : "r"(&l->lock), "r"(1u)
      : "memory");
  return fail == 0; /* 1 = acquired */
}

static inline void hyp_spin_unlock(hyp_spinlock_t *l) {
  __asm__ __volatile__(
      "stlr wzr, [%0]\n\t" /* store-release 0 */
      "sev"                /* wake any WFE waiters */
      :: "r"(&l->lock) : "memory");
}

/* IRQ-save acquire/release. MANDATORY for every lock reachable from BOTH a
 * synchronous trap handler AND the EL2 IRQ (CNTHP / reschedule-SGI) handler: a
 * core holding a lock in trap context that then takes an IRQ which wants the
 * same lock would self-deadlock. Masking I on the local core while held removes
 * that whole class of bug. Returns the prior DAIF to restore. */
static inline uint64_t hyp_lock_irqsave(hyp_spinlock_t *l) {
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #2" ::: "memory"); /* mask IRQ (I bit) */
  hyp_spin_lock(l);
  return daif;
}

static inline void hyp_unlock_irqrestore(hyp_spinlock_t *l, uint64_t daif) {
  hyp_spin_unlock(l);
  __asm__ __volatile__("msr daif, %0" :: "r"(daif) : "memory");
}

#endif /* HYP_LOCK_H */
