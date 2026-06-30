#ifndef RMM_RMI_H
#define RMM_RMI_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * rmi.h — RMI, the Realm Management Interface (Normal-world host -> RMM)
 *
 * In Arm CCA the Normal-world host (hypervisor/OS) drives the RMM through the
 * RMI ABI to create and manage Realms. It is an SMCCC interface: the host
 * issues `smc` with a command FID in x0 and arguments in x1.. , and the EL3
 * monitor routes it to the RMM running at Realm-EL2.
 *
 * In this learning fork there is no EL3/RME, so the RMM lives at EL2 and the
 * EL1 "host" (the Fermi kernel) reaches it with `hvc #0` instead of `smc`.
 * The contract is otherwise identical: command FID in x0, args x1..x3, status
 * back in x0. Command IDs live in the RMI_ range so they never collide with
 * RSI_ (Realm -> RMM) calls that share the same HVC trap path.
 *
 * The granule/realm/REC commands arrive in later milestones; this first cut
 * carries the version, liveness and introspection surface inherited from the
 * stage-2 isolation base.
 * --------------------------------------------------------------------------- */

#define RMI_BASE             0x0100UL
#define RMI_VERSION          (RMI_BASE + 0x0) /* () -> ABI version              */
#define RMI_FEATURES         (RMI_BASE + 0x1) /* () -> feature bitmap (stub)    */
#define RMI_PUTC             (RMI_BASE + 0x2) /* (char in x1) -> 0 ; debug putc */
#define RMI_PING             (RMI_BASE + 0x3) /* (v in x1) -> v + 1 ; liveness  */
#define RMI_MONITOR_INFO     (RMI_BASE + 0x4) /* () -> # RMI commands serviced  */
#define RMI_MONITOR_BASE     (RMI_BASE + 0x5) /* () -> base IPA of RMM-private  */

/* Granule lifecycle (R2). The host moves a 4 KiB page between the Normal world
 * and the RMM. A DELEGATED granule is unmapped from the host's stage-2. */
#define RMI_GRANULE_DELEGATE   (RMI_BASE + 0x10) /* (pa) -> status; host->RMM   */
#define RMI_GRANULE_UNDELEGATE (RMI_BASE + 0x11) /* (pa) -> status; RMM->host   */

#define RMI_ABI_VERSION      0x00010000ULL    /* 1.0 */

/* RMI status codes. The real RMM defines a richer enum (RMI_ERROR_INPUT,
 * RMI_ERROR_REALM, RMI_ERROR_REC, ...); we grow this set as commands land. */
#define RMI_SUCCESS              0ULL
#define RMI_ERROR_INPUT          1ULL          /* bad argument / wrong state    */
#define RMI_ERROR_NOT_SUPPORTED  ((uint64_t)-1)

#ifndef __ASSEMBLER__
/* Host-side RMI trampoline (issued from the EL1 Normal world). Mirrors the
 * SMCCC calling convention; `hvc` stands in for `smc` on this no-RME target. */
static inline uint64_t rmi_call(uint64_t cmd, uint64_t a1, uint64_t a2,
                                uint64_t a3) {
  register uint64_t x0 __asm__("x0") = cmd;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  __asm__ __volatile__("hvc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x3)
                       : "memory");
  return x0;
}
#endif

#endif /* RMM_RMI_H */
