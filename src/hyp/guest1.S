// guest1.S — a tiny second guest (Milestones 5b/7/9).
//
// Never yields: prints "[g1]\n" via HVC_PUTC then busy-spins, so only the
// hypervisor's preemption tick (CNTHP / INTID 26) slices it out.
//
// FP context-switch self-test: before the loop it parks a sentinel value in
// d5. Each iteration it re-checks d5. While guest 1 is preempted, the primary
// guest runs core::fmt, which uses d-registers — so if the hypervisor did NOT
// save/restore per-guest FP state, d5 would be corrupted and guest 1 would
// print 'X'. With correct FP context switching (M7), d5 always survives and no
// 'X' is ever emitted. It also clobbers d0..d4 each loop to stress the primary
// guest's FP state in the other direction.
//
// Lifecycle (M9): after 6 iterations it powers off via PSCI SYSTEM_OFF; the
// hypervisor reaps the vCPU and stops scheduling it.
//
// Position-independent: only relative branches and hypercalls, no memory.
// HVC_PUTC=1 inlined as a literal; must match hypercall.rs.

.section .text
.global guest1_payload
.global guest1_payload_end

guest1_payload:
	fmov	d5, #2.0		// sentinel; must survive every preemption
	mov	x20, #0			// iteration counter (survives preemption in regs[])
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

	// Verify the sentinel survived (it spans many preemptions).
	fmov	d6, #2.0
	fcmp	d5, d6
	b.eq	3f
	mov	x0, #1			// HVC_PUTC
	mov	x1, #'X'		// FP state was corrupted across a switch!
	hvc	#0
3:
	// Clobber other FP regs to stress the primary guest's FP state.
	fmov	d0, #1.5
	fmov	d1, #2.5
	fadd	d2, d0, d1
	fmul	d3, d2, d0
	fmov	d4, #-3.0

	// After 6 iterations, voluntarily power off via PSCI SYSTEM_OFF. The
	// hypervisor reaps this vCPU and stops scheduling it.
	add	x20, x20, #1
	cmp	x20, #6
	b.lt	4f
	movz	x0, #0x0008
	movk	x0, #0x8400, lsl #16	// x0 = 0x84000008 (PSCI SYSTEM_OFF)
	hvc	#0
	// not reached
4:
	// Busy-delay so output is paced; no yield.
	movz	x9, #0x600, lsl #16	// x9 = 0x06000000
2:
	subs	x9, x9, #1
	b.ne	2b

	b	1b
guest1_payload_end:
