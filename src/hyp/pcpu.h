#ifndef HYP_PCPU_H
#define HYP_PCPU_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/* ---------------------------------------------------------------------------
 * Per-physical-CPU control block.
 *
 * The hypervisor runs on up to HYP_MAX_PCPUS physical cores. State that was a
 * single global on the uniprocessor (the running vCPU, the CPU-time stamp, the
 * scheduler slice deadline) is now per-pCPU and lives here. Each core stashes a
 * pointer to ITS block in TPIDR_EL2 at bring-up, so any EL2 code can find "my
 * pCPU" with a single register read (this_pcpu()).
 *
 * The array lives in the EL2 cacheable arena (Normal-WB-IS, once the EL2 MMU is
 * on), so cross-core reads of fields like `online` are coherent.
 * ------------------------------------------------------------------------- */

#define HYP_MAX_PCPUS  4

#ifdef __ASSEMBLER__
/* Bare integer literals for asm (no ULL suffix; loaded via ldr= since they are
 * not single-mov immediates). */
#define HYP_STACK_SIZE   0x10000
#define HYP_STACK_STRIDE 0x11000
#else
#define HYP_STACK_SIZE   0x10000ULL  /* 64 KiB EL2 stack per pCPU */
#define HYP_STACK_STRIDE 0x11000ULL  /* 64 KiB stack + 4 KiB guard page */
#endif

/* sizeof(hyp_pcpu_t), needed by hyp_boot.S to index hyp_pcpus[] in asm. Kept in
 * sync with the struct below by a _Static_assert in pcpu.c. */
#define PCPU_STRUCT_SIZE 56

#ifndef __ASSEMBLER__

struct vcpu; /* forward decl (vcpu.h includes nothing of us) */

typedef struct hyp_pcpu {
  uint32_t      cpu_id;          /* 0..HYP_MAX_PCPUS-1                         */
  uint32_t      online;          /* 1 once this core has finished percpu init  */
  uint64_t      mpidr;           /* this core's real MPIDR_EL1 (affinity)      */
  struct vcpu  *current;         /* resident vCPU (was the global cur_vcpu).
                                  * WRITTEN ONLY by the owning pCPU.           */
  uint64_t      cur_load_tsc;    /* CNTPCT when `current` was loaded           */
  uint64_t      sched_deadline;  /* absolute CNTPCT of this core's next slice  */
  uint64_t      gicr_base;       /* this core's discovered redistributor frame */
  uint64_t      stack_top;       /* top of this core's EL2 stack               */
} hyp_pcpu_t;

extern hyp_pcpu_t hyp_pcpus[HYP_MAX_PCPUS];

/* "My" per-pCPU block, from TPIDR_EL2 (set at bring-up). */
static inline hyp_pcpu_t *this_pcpu(void) {
  hyp_pcpu_t *p;
  __asm__ __volatile__("mrs %0, tpidr_el2" : "=r"(p));
  return p;
}

#endif /* __ASSEMBLER__ */

#endif /* HYP_PCPU_H */
