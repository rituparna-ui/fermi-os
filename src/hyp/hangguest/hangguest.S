// hangguest: a guest (EL1) that demonstrates the LIVENESS WATCHDOG. Unlike the
// crasher (which faults), this guest HANGS — it stops making progress without
// ever taking a trap, the failure mode fault-isolation can't catch.
//
// It arms a watchdog (HVC_FERMI_WDOG, x1 = timeout ticks), pets it a few times
// while printing "alive", then enters an infinite tight spin and stops petting.
// The hypervisor's per-tick watchdog check sees the missed deadline and reboots
// this VM — so the cycle (alive x3 -> hang -> reboot) repeats, while every other
// VM keeps running. Counter in RAM is reset by the reboot, so the cycle is
// deterministic.
//
// Links at IPA 0x40000000; built flat + embedded (hangguest_blob.S).

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018
.equ WDOG_TICKS, 0x4000000        // ~1s at 62.5MHz: generous pet interval

.section .text.boot.hang
.global _hang_start
_hang_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	adr	x19, h_banner
	bl	h_puts

	// Arm the watchdog.
	mov	x1, #WDOG_TICKS
	bl	wdog

	mov	x23, #3                   // pet+print 3 times, then hang
1:	cbz	x23, 2f
	adr	x19, h_alive
	bl	h_puts
	mov	x1, #WDOG_TICKS           // pet
	bl	wdog
	// short delay between pets (well under the watchdog period)
	mov	x20, #0x800000
3:	subs	x20, x20, #1
	b.ne	3b
	sub	x23, x23, #1
	b	1b

2:	// HANG: stop petting and spin forever. The watchdog will expire and the
	// hypervisor will reboot us (back to _hang_start).
	adr	x19, h_hang
	bl	h_puts
4:	b	4b                        // tight infinite loop, no pets, no traps

// x1 = timeout ticks. Arm/pet the watchdog (HVC_FERMI_WDOG = 0xFE110004).
wdog:
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16
	movk	x0, #0x0004
	hvc	#0
	ret

h_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
5:	ldrb	w11, [x19], #1
	cbz	w11, 6f
7:	ldr	w12, [x10]
	tbnz	w12, #5, 7b
	str	w11, [x9]
	b	5b
6:	ret

.section .rodata.hang
h_banner: .asciz "\r\n[hangguest] arming watchdog, will hang after 3 pets\r\n"
h_alive:  .asciz "[hangguest] alive, petting watchdog\r\n"
h_hang:   .asciz "[hangguest] HANGING NOW (watchdog should reboot me)\r\n"
