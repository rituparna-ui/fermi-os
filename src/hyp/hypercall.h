#ifndef HYP_HYPERCALL_H
#define HYP_HYPERCALL_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * hypercall.h — Fermi hypervisor call ABI (shared by guest and hypervisor)
 *
 * Calling convention (SMCCC-like):
 *   x0 = function ID, x1..x3 = arguments, then `hvc #0`.
 *   On return, x0 = result. x1..x3 are preserved by the hypervisor.
 *
 * The function ID lives in x0 (not the HVC immediate) so guests can dispatch
 * dynamically without self-modifying code.
 * --------------------------------------------------------------------------- */

#define HVC_VERSION 0 /* () -> ABI version                                    */
#define HVC_PUTC 1    /* (char in x1) -> 0 ; paravirt console putc            */
#define HVC_PING 2    /* (val in x1) -> val + 1 ; liveness / echo            */
#define HVC_VM_INFO 3 /* () -> number of hypercalls serviced for this vCPU    */
#define HVC_YIELD 4   /* () -> 0 ; cooperative yield (stub until M5)          */
#define HVC_HYP_BASE 5 /* () -> base IPA of the hypervisor-private region     */

#define HYP_ABI_VERSION 0x00010000ULL      /* 1.0 */
#define HVC_ERR_BADCALL ((uint64_t)-1)     /* unknown function ID */

#ifndef __ASSEMBLER__
/* Guest-side hypercall trampoline. */
static inline uint64_t hvc_call(uint64_t fn, uint64_t a1, uint64_t a2,
                                uint64_t a3) {
  register uint64_t x0 __asm__("x0") = fn;
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

#endif /* HYP_HYPERCALL_H */
