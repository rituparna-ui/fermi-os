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
	// Working set SHRINKS as the counter grows: ws = clamp(80 - counter, 1, 16).
	// So during a migration the per-round dirty set decreases 16 -> ... -> 1,
	// letting iterative pre-copy actually converge.
	mov	x14, #80
	subs	x14, x14, x11          // 80 - counter (signed)
	b.gt	7f
	mov	x14, #1                 // counter >= 80 -> ws = 1
	b	8f
7:
	cmp	x14, #16
	b.le	8f
	mov	x14, #16                // clamp high to 16
8:
	mov	x13, x10                // write the counter to pages 16 .. 16+ws-1
9:
	str	x11, [x13]
	add	x13, x13, #0x1000
	subs	x14, x14, #1
	b.ne	9b
	mov	x0, #HVC_PING
	mov	x1, #0
	hvc	#0                      // bump this vCPU's hvc_count (liveness)
	wfi                            // yield (trapped -> hypervisor world-switch),
                                       // so this demo guest barely steals CPU
	b	1b
guest3_payload_end:
