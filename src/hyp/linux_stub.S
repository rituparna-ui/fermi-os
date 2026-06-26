// linux_stub.S — Linux-slot guest bring-up probe (Milestone 10).
//
// A stand-in for the real Linux Image while the memory/stage-2 model is being
// built. It runs at EL1 with the MMU off from the guest's high-RAM slice
// (guest IPA 0x40000000, which stage-2 maps to Fermi-invisible physical RAM)
// and writes 'L' directly to the PL011 UART at IPA 0x09000000 — exactly the
// path Linux's earlycon uses before any IRQ/timer setup.
//
// If 'L' appears, it proves: (1) guest code executes from the high-RAM slice,
// and (2) the guest's stage-2 device mapping of the UART works. Position-
// independent: builds all addresses with movz/movk, touches no other memory.

.section .text
.global linux_stub
.global linux_stub_end

linux_stub:
	movz	x1, #0x0900, lsl #16	// x1 = 0x09000000 (PL011 base, IPA)
	add	x3, x1, #0x18		// x3 = FR (flag register)
1:
	// Wait while TX FIFO full (FR.TXFF = bit 5).
2:
	ldr	w2, [x3]
	tbnz	w2, #5, 2b
	mov	w2, #'L'
	str	w2, [x1]		// UART_DR <- 'L'

	// Pace the output with a busy delay; preemption interrupts this.
	movz	x9, #0x800, lsl #16	// ~0x08000000 iterations
3:
	subs	x9, x9, #1
	b.ne	3b

	b	1b
linux_stub_end:
