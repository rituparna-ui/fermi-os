#ifndef HYP_HYP_H
#define HYP_HYP_H

#include <stdint.h>

/* FermiOS EL2 hypervisor — milestone 2: dedicated EL2 trap plumbing.
 *
 * The host kernel runs at EL2 under VHE (HCR_EL2.E2H=1, set in boot.S). Its
 * own exceptions go through the redirected VBAR_EL1 (== VBAR_EL2) table in
 * src/exception/vector.S. This module provides a SEPARATE EL2 vector table
 * that reads the EL2-banked syndrome registers (ELR/SPSR/ESR/HPFAR/FAR_EL2)
 * and is the future guest-exit hot path: the world switch will install it in
 * VBAR_EL2 before entering a guest and restore the host table on exit. */

/* Exception type tags passed by the vector stubs (mirror EXCEPTION_* in
 * exception.h so the two dispatchers stay legible side by side). */
#define HYP_EXC_SYNC   0
#define HYP_EXC_IRQ    1
#define HYP_EXC_FIQ    2
#define HYP_EXC_SERROR 3

/* EL2 trap frame. MUST match the store/load offsets in hyp_vectors.S.
 *   regs[0..30]  x0..x30            (248 bytes)
 *   elr          ELR_EL2            (+248)
 *   spsr         SPSR_EL2           (+256)
 *   esr          ESR_EL2            (+264)
 *   hpfar        HPFAR_EL2          (+272)  faulting IPA[47:12]<<4 on S2 abort
 *   far          FAR_EL2            (+280)
 * Total 288 bytes (16-byte aligned). */
typedef struct hyp_trap_frame {
  uint64_t regs[31];
  uint64_t elr;
  uint64_t spsr;
  uint64_t esr;
  uint64_t hpfar;
  uint64_t far;
} hyp_trap_frame_t;

#define HYP_FRAME_SIZE 288

/* True if currently executing at EL2 (the VHE host); false at EL1 (legacy
 * boot or running as a guest). */
int hyp_at_el2(void);

/* Bring up the EL2 trap layer: probe EL2/VHE, and (milestone 2) run a
 * self-test that routes a host-issued HVC through the dedicated EL2 vector
 * table to prove syndrome capture works and the host survives. No-op at EL1. */
void hyp_init(void);

/* C entry from hyp_exception_common. type is one of HYP_EXC_*. */
void hyp_dispatch(uint64_t type, hyp_trap_frame_t *frame);

/* The dedicated EL2 vector table (defined in hyp_vectors.S). */
extern char hyp_vector_table[];

/* Milestone 3 smoke test: build a stage-2, load the trivial EL1 guest stub,
 * world-switch into it, and verify the HVC exit + guest RAM write. No-op at
 * EL1. Must run before the host timer is started (it uses no preemption). */
void hyp_run_smoke_guest(void);

/* Milestone 4 demo: run a spinning EL1 guest preempted by the EL2 physical
 * timer (CNTHP, PPI 26) for a fixed number of slices, proving IRQ-driven
 * exit + guest resume. No-op at EL1. Runs before the host timer starts. */
void hyp_run_timeslice_demo(void);

#endif /* HYP_HYP_H */
