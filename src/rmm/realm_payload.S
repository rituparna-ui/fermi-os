/* ---------------------------------------------------------------------------
 * realm_payload.S — Realm payload (R4b)
 *
 * Runs at EL1 under the realm's private stage-2 (RTT) with the stage-1 MMU
 * OFF, so it executes and reads its data at the mapped IPA. Position-
 * independent (adr is PC-relative; PC == IPA here). Exercises the RSI run
 * loop:
 *   phase 1 : print a line one char at a time via RSI_PUTC (each HVC is
 *             serviced by the RMM in place and the realm is resumed)
 *   host call: RSI_HOST_CALL exits to the Normal-world host, which re-enters
 *   phase 2 : print a second line, then RSI_EXIT to finish.
 *
 * Registers used: x0 (RSI fn / result), x1 (arg / char), x19 (string cursor).
 * el2_common saves/restores all of x0..x30 around every HVC, so x19 survives
 * across the in-place RSI services.
 * --------------------------------------------------------------------------- */

#include "rmm/rsi.h"

.section .text.realm, "ax"
.global realm_payload
.global realm_payload_end

realm_payload:
	/* phase 1: print msg1 */
	adr	x19, msg1
1:	ldrb	w1, [x19], #1
	cbz	w1, 2f
	mov	x0, #RSI_PUTC
	hvc	#0
	b	1b

2:	/* host call (exits to host; host re-enters to resume us) */
	mov	x0, #RSI_HOST_CALL
	movz	x1, #0xC0DE
	hvc	#0

	/* phase 2: resumed by the host — print msg2 */
	adr	x19, msg2
3:	ldrb	w1, [x19], #1
	cbz	w1, 4f
	mov	x0, #RSI_PUTC
	hvc	#0
	b	3b

4:	/* done */
	mov	x0, #RSI_EXIT
	hvc	#0
5:	wfi
	b	5b

msg1:
	.asciz "[realm] phase 1: running under my own stage-2 (RSI_PUTC)\n"
msg2:
	.asciz "[realm] phase 2: resumed by host re-entry; exiting\n"
.balign 8
realm_payload_end:
