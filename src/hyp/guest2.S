/* ---------------------------------------------------------------------------
 * guest2.S — a third bare-metal guest (Milestone 16, N>2 multi-VM)
 *
 * A self-contained, position-independent EL1 payload that demonstrates the
 * vCPU table / scheduler / per-guest stage-2 scale beyond two guests, WITHOUT
 * adding serial noise (we keep Fermi's console clean). Each loop it issues a
 * HVC_PING — which bumps this vCPU's hypercall counter in the hypervisor — then
 * busy-spins. Its progress is therefore visible purely through introspection:
 * `cat /proc/vms` shows vCPU 2's HVC count climbing as it is time-sliced.
 *
 * Touches no memory and uses only relative branches + hypercalls, so it runs
 * unmodified wherever the hypervisor copies it.
 * --------------------------------------------------------------------------- */

#include "hyp/hypercall.h"

.section .text
.global guest2_payload
.global guest2_payload_end

guest2_payload:
1:
	mov	x0, #HVC_PING
	mov	x1, #0
	hvc	#0
	movz	x9, #0x400, lsl #16   // ~0x04000000 iterations
2:
	subs	x9, x9, #1
	b.ne	2b
	b	1b
guest2_payload_end:
