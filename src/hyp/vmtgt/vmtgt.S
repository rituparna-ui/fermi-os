// vmtgt: a tiny "migration target" guest (EL1). It boots, prints one banner,
// then idles by yielding. It is the DESTINATION for a live migration: dom0
// snapshots another VM (guest2) and clones that snapshot into this slot via
// VMCTL_MIGRATE. After the migration this vCPU's RAM + registers ARE guest2's,
// so it resumes executing guest2's code (continuing the beat counter) — but in
// this slot's own stage-2 (different host PA + VMID). Proof that guest state is
// host-PA / VMID agnostic.
//
// It must have the SAME ram_size as guest2 (64 MiB) so the snapshot fits. Links
// at IPA 0x40000000 like every guest; built flat + embedded (vmtgt_blob.S).

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018
.equ HVC_YIELD, 0xFE110000

.section .text.boot.vmtgt
.global _vmtgt_start
_vmtgt_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	adr	x19, vt_banner
	bl	vt_puts
1:	// idle: yield forever until dom0 migrates guest2 into this slot, after
	// which execution resumes inside guest2's code instead of here.
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16	// HVC_YIELD
	hvc	#0
	b	1b

// x19 = NUL-terminated string. Clobbers x9,x10,x11,x12.
vt_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
2:	ldrb	w11, [x19], #1
	cbz	w11, 3f
4:	ldr	w12, [x10]
	tbnz	w12, #5, 4b
	str	w11, [x9]
	b	2b
3:	ret

.section .rodata.vmtgt
vt_banner:
	.asciz "\r\n[vmtgt] migration target idle, awaiting a clone...\r\n"
