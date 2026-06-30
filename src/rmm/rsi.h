#ifndef RMM_RSI_H
#define RMM_RSI_H

/* ---------------------------------------------------------------------------
 * rsi.h — RSI, the Realm Services Interface (Realm -> RMM)
 *
 * The counterpart of RMI: where the Normal-world host drives the monitor via
 * RMI, code running *inside* a realm calls *up* to the RMM via RSI. Same HVC
 * trap path; the RMM tells the two apart because it knows when a realm is the
 * running world. RSI command IDs live in their own range.
 *
 * Two classes of RSI call:
 *   - serviced in place: the RMM handles the request and re-enters the realm
 *     immediately (e.g. RSI_VERSION, RSI_REALM_CONFIG, RSI_PUTC). The realm
 *     never leaves; the host isn't involved.
 *   - exit to host: the call needs the Normal world, so the RMM world-switches
 *     back to the host and RMI_REC_ENTER returns a reason (RSI_HOST_CALL,
 *     RSI_EXIT). The host acts, then re-enters the REC to resume the realm.
 *
 * Command IDs are plain integers so this header is usable from both C and the
 * realm payload assembly (-x assembler-with-cpp).
 * --------------------------------------------------------------------------- */

#define RSI_BASE          0x200
#define RSI_VERSION       (RSI_BASE + 0x0) /* () -> RSI ABI version           */
#define RSI_REALM_CONFIG  (RSI_BASE + 0x1) /* () -> realm config (vmid here)  */
#define RSI_PUTC          (RSI_BASE + 0x2) /* (char in x1) paravirt console   */
#define RSI_HOST_CALL     (RSI_BASE + 0x3) /* (arg in x1) -> exit to host     */
#define RSI_EXIT          (RSI_BASE + 0x4) /* () realm is done -> exit to host*/
#define RSI_ATTESTATION_TOKEN (RSI_BASE + 0x5) /* (challenge in x1) -> token lo*/

#ifndef __ASSEMBLER__
#include <stdint.h>

#define RSI_ABI_VERSION   0x00010000ULL /* 1.0 */

/* Realm-side RSI trampoline (issued from inside a realm at EL1). */
static inline uint64_t rsi_call(uint64_t fn, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fn;
  register uint64_t x1 __asm__("x1") = a1;
  __asm__ __volatile__("hvc #0" : "+r"(x0) : "r"(x1) : "memory");
  return x0;
}
#endif

#endif /* RMM_RSI_H */
