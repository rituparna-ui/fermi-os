// VM2: a tiny standalone EL1 guest used to demonstrate the multi-VM world
// switch and per-VM stage-2 isolation. It links/runs at IPA 0x40000000 — the
// SAME guest-physical address FermiOS (VM1) uses — but the hypervisor maps
// VM2's stage-2 to a DIFFERENT host PA, so the two cannot see each other.
//
// It does almost nothing: set up a stack, then loop printing a heartbeat to
// the PL011 UART (mapped Device-nGnRE in VM2's stage-2) with a delay. It never
// touches the GIC or timer — interrupts stay masked — so it is a pure compute
// + MMIO guest. The EL2 scheduler preempts it on the hypervisor's own tick.
//
// Built into a flat blob and embedded in the hyp (guest2_blob.S), same as VM1.

.section .text.boot.g2
.global _g2_start
_g2_start:
	// Stack at top of VM2's low RAM view (IPA 0x40200000 = 2 MiB in).
	ldr	x0, =0x40200000
	mov	sp, x0

	// Print a one-time banner: "VM2: hello from guest 2\n"
	adr	x19, g2_banner
	bl	g2_puts

	// Heartbeat counter in writable RAM (IPA 0x40210000). On a warm reset the
	// hypervisor re-copies our pristine image and re-zeroes .bss-style state,
	// but this scratch word lives in plain RAM, so we initialise it explicitly.
	ldr	x21, =0x40210000
	str	xzr, [x21]

1:	// Heartbeat loop — increment the counter in RAM and print "beat NN" via the
	// PV console. The counter lives in plain guest RAM, so it is part of the VM
	// snapshot: dom0 can snapshot us at beat N, let us climb, then RESTORE — and
	// the count visibly jumps back to N, proving the rollback.
	ldr	x21, =0x40210000
	ldr	x22, [x21]
	add	x22, x22, #1
	str	x22, [x21]

	adr	x19, g2_beat
	bl	g2_puts                 // "[guest2] beat " (raw UART so dom0 logs interleave)
	and	w23, w22, #0xFF
	bl	g2_puthex2              // the beat number
	adr	x19, g2_nl
	bl	g2_puts

	// crude delay
	mov	x20, #0x4000000
2:	subs	x20, x20, #1
	bne	2b
	// Cooperatively yield the rest of our time slice to the other VM via a
	// hypervisor "yield" hypercall (a non-PSCI HVC the hyp recognises).
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16	// x0 = 0xFE110000 — Fermi hyp YIELD id
	hvc	#0
	b	1b

// x19 = NUL-terminated string. Clobbers x9,x10,x11.
g2_puts:
	ldr	x9, =0x09000000		// UART DR
	ldr	x10, =0x09000018	// UART FR
3:	ldrb	w11, [x19], #1
	cbz	w11, 4f
5:	ldr	w12, [x10]		// FR
	tbnz	w12, #5, 5b		// TXFF set -> wait
	str	w11, [x9]
	b	3b
4:	ret

// print byte in w23 as two hex digits. Clobbers x9,x10,x12,x15.
g2_puthex2:
	ldr	x9, =0x09000000
	ldr	x10, =0x09000018
	lsr	w15, w23, #4
	and	w15, w15, #0xF
	cmp	w15, #10
	b.lt	6f
	add	w15, w15, #('A' - 10)
	b	7f
6:	add	w15, w15, #'0'
7:	ldr	w12, [x10]
	tbnz	w12, #5, 7b
	str	w15, [x9]
	and	w15, w23, #0xF
	cmp	w15, #10
	b.lt	8f
	add	w15, w15, #('A' - 10)
	b	9f
8:	add	w15, w15, #'0'
9:	ldr	w12, [x10]
	tbnz	w12, #5, 9b
	str	w15, [x9]
	ret

.section .rodata.g2
g2_banner:
	.asciz "\r\n[VM2] hello from guest 2 (EL1, isolated stage-2)\r\n"
g2_beat:
	.asciz "[guest2] beat "
g2_nl:
	.asciz "\r\n"
