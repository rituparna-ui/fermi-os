/* ---------------------------------------------------------------------------
 * guest3.S — a small migratable bare-metal guest (live-migration demo)
 *
 * A position-independent EL1 payload that maintains a 64-bit counter in its own
 * RAM (at IPA 0x40010000, well past its code page) and bumps it every loop,
 * issuing an HVC_PING each iteration so the hypervisor can see it is alive.
 *
 * The counter lets the hypervisor prove a *live migration*: it reads the
 * counter from the guest's current physical backing before and after relocating
 * the guest's RAM to a new physical window. Because the guest actively writes
 * the counter page each loop, it also exercises stage-2 dirty-page tracking
 * during pre-copy.
 * --------------------------------------------------------------------------- */

#include "hyp/hypercall.h"

.section .text
.global guest3_payload
.global guest3_payload_end

guest3_payload:
	movz	x10, #0x4001, lsl #16   // x10 = 0x40010000 : counter / working-set base
	str	xzr, [x10]              // counter = 0
1:
	ldr	x11, [x10]
	add	x11, x11, #1            // counter++
	// Write the counter across a 16-page working set (pages 16..31) so the
	// hypervisor's iterative pre-copy has a non-trivial dirty set to track.
	mov	x12, #16
	mov	x13, x10
3:
	str	x11, [x13]
	add	x13, x13, #0x1000
	subs	x12, x12, #1
	b.ne	3b
	mov	x0, #HVC_PING
	mov	x1, #0
	hvc	#0                      // bump this vCPU's hvc_count (liveness)
	movz	x9, #0x40, lsl #16      // brief busy-spin between iterations
2:
	subs	x9, x9, #1
	b.ne	2b
	b	1b
guest3_payload_end:
