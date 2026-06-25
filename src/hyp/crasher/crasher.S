// crasher: a guest (EL1) that deliberately faults, to demonstrate per-VM fault
// isolation. It prints a banner + a beat or two, then executes an instruction
// that traps to EL2 with no valid handler (an access to a stage-2-unmapped IPA
// far outside its RAM window). The hypervisor reboots JUST this VM (up to its
// fault budget, then powers it off) while every other VM keeps running.
//
// Counter in RAM survives the reboot? No — warm-reset re-copies the pristine
// image, so the counter restarts from 0 each reboot, and we crash again, which
// is exactly how we exercise the fault budget -> eventual power-off.
//
// Links at IPA 0x40000000; built flat + embedded (crasher_blob.S).

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.section .text.boot.crasher
.global _crasher_start
_crasher_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	adr	x19, c_banner
	bl	c_puts

	// Touch an IPA far outside our 16 MiB RAM window and any device window:
	// 0x70000000 is stage-2 INVALID, so this load faults to EL2 as a data
	// abort the dispatcher cannot service -> hyp_fatal_trap -> fault isolate.
	adr	x19, c_crash
	bl	c_puts
	ldr	x1, =0x70000000
	ldr	x2, [x1]              // <-- stage-2 fault, traps to EL2

	// not reached (the hyp reboots us at _crasher_start)
	adr	x19, c_after
	bl	c_puts
1:	wfe
	b	1b

c_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
2:	ldrb	w11, [x19], #1
	cbz	w11, 3f
4:	ldr	w12, [x10]
	tbnz	w12, #5, 4b
	str	w11, [x9]
	b	2b
3:	ret

.section .rodata.crasher
c_banner: .asciz "\r\n[crasher] alive — about to fault\r\n"
c_crash:  .asciz "[crasher] dereferencing unmapped IPA 0x70000000...\r\n"
c_after:  .asciz "[crasher] !!! continued past fault (unexpected) !!!\r\n"
