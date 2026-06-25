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

1:	// Heartbeat loop
	adr	x19, g2_beat
	bl	g2_puts

	// After 5 heartbeats, request a PSCI SYSTEM_RESET. The hypervisor warm-
	// resets THIS VM only: it reloads our image and restarts us from _g2_start,
	// so the banner prints again and the counter restarts. (VM1 is unaffected.)
	ldr	x21, =0x40210000
	ldr	x22, [x21]
	add	x22, x22, #1
	str	x22, [x21]
	cmp	x22, #5
	b.lt	3f
	adr	x19, g2_reset_msg
	bl	g2_puts
	mov	x0, #0
	movk	x0, #0x8400, lsl #16	// x0 = 0x84000009 = PSCI SYSTEM_RESET
	movk	x0, #0x0009
	hvc	#0
	// not reached — the hyp re-enters us at _g2_start
3:
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

.section .rodata.g2
g2_banner:
	.asciz "\r\n[VM2] hello from guest 2 (EL1, isolated stage-2)\r\n"
g2_beat:
	.asciz "[VM2] heartbeat\r\n"
g2_reset_msg:
	.asciz "[VM2] 5 beats -> PSCI SYSTEM_RESET (warm restart)\r\n"
