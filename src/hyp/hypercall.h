#ifndef HYP_HYPERCALL_H
#define HYP_HYPERCALL_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

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
#define HVC_YIELD 4   /* () -> 0 ; cooperative yield                          */
#define HVC_HYP_BASE 5 /* () -> base IPA of the hypervisor-private region     */
#define HVC_VM_COUNT 6 /* () -> number of vCPUs                                */
#define HVC_VM_STAT 7  /* (id in x1, field in x2) -> stat value, or -1         */
#define HVC_LCON_LEN 8 /* () -> bytes captured in the Linux console buffer     */
#define HVC_LCON_GET 9 /* (offset in x1) -> up to 8 console bytes, packed LE   */
#define HVC_LCON_PUT 10 /* (byte in x1) -> push one input byte to Linux's UART RX */
#define HVC_VM_CTL 11   /* (op in x1, id in x2) -> 0 / -1 ; guest lifecycle control */

/* Operation codes for HVC_VM_CTL. */
#define VMCTL_PAUSE 0    /* pause (deschedule) a vCPU            */
#define VMCTL_RESUME 1   /* resume a paused vCPU                 */
#define VMCTL_MIGRATE 2  /* arm a live migration of a vCPU       */
#define VMCTL_SNAPSHOT 3 /* checkpoint a vCPU's state            */
#define VMCTL_RESTORE 4  /* restore a vCPU to its checkpoint     */

/* Field selectors for HVC_VM_STAT. */
#define VMSTAT_ID 0
#define VMSTAT_STATE 1     /* 0=unused, 1=ready, 2=running */
#define VMSTAT_HVC 2
#define VMSTAT_SYSREG 3
#define VMSTAT_ABORT 4
#define VMSTAT_VIRQ 5
#define VMSTAT_SWITCHES 6  /* global world-switch count (same for all ids) */
#define VMSTAT_MMIO 7      /* emulated guest MMIO accesses */
#define VMSTAT_WFI 8       /* global WFI idle-yield count (same for all ids) */

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
