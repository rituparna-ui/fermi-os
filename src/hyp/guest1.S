/* ---------------------------------------------------------------------------
 * guest1.S — a tiny second guest (Milestone 5a)
 *
 * A self-contained EL1 payload that uses ONLY hypercalls: it prints "[g1]\n"
 * via HVC_PUTC and then cooperatively yields (HVC_YIELD) back to the
 * hypervisor, forever. It touches no memory and uses only relative branches,
 * so it is fully position-independent — the hypervisor copies it into guest
 * 1's RAM slice and runs it there with the stage-1 MMU off.
 * --------------------------------------------------------------------------- */

#include "hyp/hypercall.h"

.section .text
.global guest1_payload
.global guest1_payload_end

guest1_payload:
1:
	mov	x0, #HVC_PUTC
	mov	x1, #'['
	hvc	#0
	mov	x0, #HVC_PUTC
	mov	x1, #'g'
	hvc	#0
	mov	x0, #HVC_PUTC
	mov	x1, #'1'
	hvc	#0
	mov	x0, #HVC_PUTC
	mov	x1, #']'
	hvc	#0
	mov	x0, #HVC_PUTC
	mov	x1, #10            // '\n'
	hvc	#0

	mov	x0, #HVC_YIELD
	hvc	#0

	b	1b
guest1_payload_end:
