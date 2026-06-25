// guest1.S — a tiny second guest (Milestone 5b).
//
// A self-contained EL1 payload that NEVER yields: it prints "[g1]\n" via
// HVC_PUTC, then spins in a delay loop, forever. Because it never cooperates,
// the only thing that takes the CPU away from it is the hypervisor's preemptive
// scheduling tick (CNTHP / INTID 26). Position-independent: only relative
// branches and hypercalls, touches no memory.
//
// HVC_PUTC=1 is inlined as a literal to keep the payload free of any
// include/relocation; it must match hypercall.rs.

.section .text
.global guest1_payload
.global guest1_payload_end

guest1_payload:
1:
	mov	x0, #1			// HVC_PUTC
	mov	x1, #'['
	hvc	#0
	mov	x0, #1			// HVC_PUTC
	mov	x1, #'g'
	hvc	#0
	mov	x0, #1			// HVC_PUTC
	mov	x1, #'1'
	hvc	#0
	mov	x0, #1			// HVC_PUTC
	mov	x1, #']'
	hvc	#0
	mov	x0, #1			// HVC_PUTC
	mov	x1, #10			// '\n'
	hvc	#0

	// Busy-delay (~tens of millions of iterations) so output is paced. No
	// yield: only the hypervisor's preemption tick interrupts this.
	movz	x9, #0x600, lsl #16	// x9 = 0x06000000
2:
	subs	x9, x9, #1
	b.ne	2b

	b	1b
guest1_payload_end:
