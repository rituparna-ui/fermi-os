// Inter-VM IPC demo guest (EL1). One image serves two roles, selected by x0
// (set by the hypervisor at entry): x0==2 -> producer, x0==3 -> consumer.
//
// The hypervisor maps ONE host page into both VMs' stage-2 at the shared IPA
// 0x50000000, while each VM's RAM (IPA 0x40000000) maps to a DIFFERENT host PA.
// So the shared page is visible to both, but their private memory is isolated.
//
//   producer: seq++ in the shared page, prints "producer wrote seq=NN", delays.
//   consumer: polls the shared seq; on change prints "consumer read  seq=NN".
//
// Matching NN across the two VMs proves genuine cross-VM shared memory. Both
// yield their slice via the Fermi yield hypercall (HVC x0=0xFE110000).
//
// Built to a flat blob and embedded in the hyp (ipc_blob.S); links at IPA
// 0x40000000 like the other guests (the hyp maps it to a per-VM host PA).

.equ UART_DR,     0x09000000
.equ UART_FR,     0x09000018
.equ SHARED_BASE, 0x50000000     // shared page IPA
.equ SEQ_OFF,     8              // seqno offset within the shared page

.section .text.boot.ipc
.global _ipc_start
_ipc_start:
	mov	x24, x0               // role: 2 = producer, 3 = consumer
	ldr	x0, =0x40200000       // private stack (in this VM's private RAM)
	mov	sp, x0
	ldr	x25, =SHARED_BASE
	mov	w26, #0               // consumer's last-seen seq

	cmp	x24, #2
	b.eq	producer
	// fall through to consumer

consumer:
	ldr	w0, [x25, #SEQ_OFF]
	cmp	w0, w26
	b.eq	c_yield
	mov	w26, w0
	adr	x19, msg_cons
	bl	ipc_puts
	and	w23, w26, #0xFF
	bl	ipc_puthex2
	adr	x19, msg_nl
	bl	ipc_puts
c_yield:
	bl	ipc_yield
	b	consumer

producer:
	ldr	w0, [x25, #SEQ_OFF]
	add	w0, w0, #1
	str	w0, [x25, #SEQ_OFF]
	dsb	sy                    // publish the write to the other VM
	adr	x19, msg_prod
	bl	ipc_puts
	and	w23, w0, #0xFF
	bl	ipc_puthex2
	adr	x19, msg_nl
	bl	ipc_puts
	mov	x20, #0x6000000       // crude delay
1:	subs	x20, x20, #1
	b.ne	1b
	bl	ipc_yield
	b	producer

// Yield the rest of this slice to another VM. Clobbers x0.
ipc_yield:
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16  // 0xFE110000 = Fermi yield hypercall
	hvc	#0
	ret

// Print NUL-terminated string at x19. Clobbers x9,x10,x11,x12.
ipc_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
2:	ldrb	w11, [x19], #1
	cbz	w11, 3f
4:	ldr	w12, [x10]
	tbnz	w12, #5, 4b           // TXFF set -> wait
	str	w11, [x9]
	b	2b
3:	ret

// Print the byte in w23 as two hex digits. Uses no nested calls (x30 safe).
ipc_puthex2:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
	// high nibble
	lsr	w15, w23, #4
	and	w15, w15, #0xF
	cmp	w15, #10
	b.lt	10f
	add	w15, w15, #('A' - 10)
	b	11f
10:	add	w15, w15, #'0'
11:	ldr	w12, [x10]
	tbnz	w12, #5, 11b
	str	w15, [x9]
	// low nibble
	and	w15, w23, #0xF
	cmp	w15, #10
	b.lt	12f
	add	w15, w15, #('A' - 10)
	b	13f
12:	add	w15, w15, #'0'
13:	ldr	w12, [x10]
	tbnz	w12, #5, 13b
	str	w15, [x9]
	ret

.section .rodata.ipc
msg_prod:
	.asciz "[IPC] producer wrote seq="
msg_cons:
	.asciz "[IPC] consumer read  seq="
msg_nl:
	.asciz "\r\n"
