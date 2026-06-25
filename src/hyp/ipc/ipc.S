// Inter-VM IPC demo guest (EL1), EVENT-DRIVEN via a hypervisor doorbell.
// One image serves two roles, selected by x0 (set by the hypervisor at entry):
//   x0 == 2 -> producer, x0 == 3 -> consumer.
//
// The hypervisor maps ONE host page into both VMs' stage-2 at the shared IPA
// 0x50000000, while each VM's private RAM (IPA 0x40000000) maps to a DIFFERENT
// host PA — shared data, isolated private memory.
//
//   producer: seq++ in the shared page, prints it, then RINGS THE DOORBELL
//             (HVC 0xFE110001). The hypervisor injects vINTID 40 into the
//             consumer. Then delays + yields.
//   consumer: sets up its GICv3 CPU interface + an EL1 IRQ vector, enables the
//             doorbell INTID, unmasks IRQs, and WFIs. On the doorbell IRQ it
//             reads the shared seq, prints it, EOIs, and WFIs again. It NEVER
//             polls — it is purely event-driven (Xen-event-channel style).
//
// Matching NN across producer/consumer proves cross-VM shared memory; the
// consumer printing only on the doorbell proves event delivery.

.equ UART_DR,     0x09000000
.equ UART_FR,     0x09000018
.equ SHARED_BASE, 0x50000000
.equ SEQ_OFF,     8

.equ GICD_BASE,   0x08000000
.equ GICD_ISENABLER, 0x0100        // GICD_ISENABLER[n], n = intid/32
.equ GICR_BASE,   0x080A0000
.equ GICR_WAKER,  0x0014
.equ DOORBELL_INTID, 40

.equ HVC_YIELD,    0xFE110000
.equ HVC_DOORBELL, 0xFE110001

// The hypervisor enters this image at its byte 0 (IPA 0x40000000), so the very
// first instruction must reach _ipc_start. The vector table needs 2 KiB
// alignment, so we branch over it from offset 0.
.section .text.boot.ipc
.global _entry
_entry:
	b	_ipc_start

// ---- EL1 vector table (consumer). 2KB aligned, 16 entries 128B apart. The
// consumer runs at EL1h and takes IRQs at the CURRENT EL with SP_ELx, which is
// entry index 5 (offset 0x280). Every other entry just eret's (unused). ----
.balign 2048
ipc_vectors:
	// Group 0: Current EL with SP0 (unused at EL1h)
	.balign 128; eret              // 0 sync
	.balign 128; eret              // 1 irq
	.balign 128; eret              // 2 fiq
	.balign 128; eret              // 3 serror
	// Group 1: Current EL with SP_ELx (EL1h — the consumer's running state)
	.balign 128; eret              // 4 sync
	.balign 128; b ipc_irq_handler // 5 IRQ  <- doorbell lands here
	.balign 128; eret              // 6 fiq
	.balign 128; eret              // 7 serror
	// Group 2: Lower EL AArch64 (unused — no EL0 in this guest)
	.balign 128; eret              // 8
	.balign 128; eret              // 9
	.balign 128; eret              // 10
	.balign 128; eret              // 11
	// Group 3: Lower EL AArch32 (unused)
	.balign 128; eret              // 12
	.balign 128; eret              // 13
	.balign 128; eret              // 14
	.balign 128; eret              // 15

.global _ipc_start
_ipc_start:
	mov	x24, x0                   // role
	ldr	x0, =0x40200000           // private stack
	mov	sp, x0
	ldr	x25, =SHARED_BASE
	cmp	x24, #2
	b.eq	producer
	b	consumer

// ---------------------------------------------------------------- producer
producer:
	// Wait until the consumer has armed its interrupt path (it sets the ready
	// flag at shared[0] = 1). This avoids ringing the doorbell before the
	// consumer's vGIC interface is up, which would leave a half-handled IRQ.
	ldr	w0, [x25]                 // shared[0] = ready flag
	cbz	w0, p_wait
	b	p_ring
p_wait:
	bl	ipc_yield
	b	producer
p_ring:
	ldr	w0, [x25, #SEQ_OFF]
	add	w0, w0, #1
	str	w0, [x25, #SEQ_OFF]
	dsb	sy                        // publish before notifying
	adr	x19, msg_prod
	bl	ipc_puts
	and	w23, w0, #0xFF
	bl	ipc_puthex2
	adr	x19, msg_nl
	bl	ipc_puts
	// Ring the doorbell -> hypervisor injects vINTID 40 into the consumer.
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16
	movk	x0, #0x0001
	hvc	#0
	mov	x20, #0x6000000           // delay
1:	subs	x20, x20, #1
	b.ne	1b
	bl	ipc_yield
	b	producer

// ---------------------------------------------------------------- consumer
consumer:
	// VBAR_EL1 -> our vector table.
	adr	x0, ipc_vectors
	msr	vbar_el1, x0
	isb
	// GICv3 CPU interface (system-register): SRE=1, PMR=0xFF, group1 enable.
	mrs	x0, S3_0_C12_C12_5        // ICC_SRE_EL1
	orr	x0, x0, #1
	msr	S3_0_C12_C12_5, x0
	isb
	mov	x0, #0xFF
	msr	S3_0_C4_C6_0, x0          // ICC_PMR_EL1
	mov	x0, #1
	msr	S3_0_C12_C12_7, x0        // ICC_IGRPEN1_EL1
	isb
	// Wake our redistributor (emulated GICR_WAKER reads 0 -> ChildrenAsleep=0).
	ldr	x0, =GICR_BASE
	ldr	w1, [x0, #GICR_WAKER]
	and	w1, w1, #~2               // clear ProcessorSleep
	str	w1, [x0, #GICR_WAKER]
	// Enable the doorbell INTID 40 in the distributor: ISENABLER[1] bit (40-32).
	ldr	x0, =GICD_BASE
	mov	w1, #(1 << (DOORBELL_INTID - 32))
	str	w1, [x0, #(GICD_ISENABLER + 4)]   // ISENABLER1 (intids 32..63)
	// Unmask IRQs, announce readiness to the producer (shared[0] = 1), wait.
	msr	daifclr, #2
	adr	x19, msg_cwait
	bl	ipc_puts
	mov	w0, #1
	str	w0, [x25]                 // shared[0] = ready
	dsb	sy
2:	wfi
	b	2b

// EL1 IRQ handler: read INTID via ICC_IAR1_EL1, handle doorbell, EOI.
ipc_irq_handler:
	mrs	x9, S3_0_C12_C12_0        // ICC_IAR1_EL1 (ack)
	// Stash the FULL IAR value in a register the UART helpers don't touch
	// (x18). ipc_puts/ipc_puthex2 clobber x9-x12; using x9 for the EOI after
	// printing would EOI a garbage INTID and leave the real one Active forever.
	mov	x18, x9
	and	x10, x9, #0xFFFFFF
	cmp	x10, #DOORBELL_INTID
	b.ne	3f
	// Doorbell: read the shared seq and print it.
	ldr	x25, =SHARED_BASE
	ldr	w23, [x25, #SEQ_OFF]
	and	w23, w23, #0xFF
	adr	x19, msg_cons
	bl	ipc_puts_irq
	bl	ipc_puthex2_irq
	adr	x19, msg_nl
	bl	ipc_puts_irq
3:	msr	S3_0_C12_C12_1, x18       // ICC_EOIR1_EL1 (EOI the acked INTID)
	eret

ipc_yield:
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16      // HVC_YIELD
	hvc	#0
	ret

// ---- UART helpers. Two copies (x30-clobber-safe variants for IRQ ctx). ----
ipc_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
4:	ldrb	w11, [x19], #1
	cbz	w11, 5f
6:	ldr	w12, [x10]
	tbnz	w12, #5, 6b
	str	w11, [x9]
	b	4b
5:	ret

ipc_puthex2:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
	lsr	w15, w23, #4
	and	w15, w15, #0xF
	cmp	w15, #10
	b.lt	7f
	add	w15, w15, #('A' - 10)
	b	8f
7:	add	w15, w15, #'0'
8:	ldr	w12, [x10]
	tbnz	w12, #5, 8b
	str	w15, [x9]
	and	w15, w23, #0xF
	cmp	w15, #10
	b.lt	9f
	add	w15, w15, #('A' - 10)
	b	10f
9:	add	w15, w15, #'0'
10:	ldr	w12, [x10]
	tbnz	w12, #5, 10b
	str	w15, [x9]
	ret

// IRQ-context copies use x16 as link so eret-path x30 is preserved.
ipc_puts_irq:
	mov	x16, x30
	bl	ipc_puts
	mov	x30, x16
	ret
ipc_puthex2_irq:
	mov	x17, x30
	bl	ipc_puthex2
	mov	x30, x17
	ret

.section .rodata.ipc
msg_prod:
	.asciz "[IPC] producer wrote seq="
msg_cons:
	.asciz "[IPC] consumer got doorbell, seq="
msg_cwait:
	.asciz "[IPC] consumer waiting for doorbell events...\r\n"
msg_nl:
	.asciz "\r\n"
