/* ---------------------------------------------------------------------------
 * vector_el3.S — EL3 exception vector table
 *
 * Standard ARMv8-A 16-entry layout (4 groups x 4 kinds, 0x80 bytes each,
 * 0x800-aligned). An SMC from a lower EL (the RMM at Realm-EL2, or later the
 * Non-secure host) lands in the "lower EL, AArch64" sync slot (group 2,
 * offset 0x400). Each stub saves x0/x1, then branches to el3_common which
 * saves the rest, calls el3_sync_handler(frame), restores, and ERETs — to
 * whatever ELR_EL3/SPSR_EL3/SCR_EL3 the handler left in place (this is how
 * the monitor switches worlds on the way out).
 * --------------------------------------------------------------------------- */

.equ EL3_FRAME, 256

.macro EL3_ENTRY
.balign 0x80
	sub	sp, sp, #EL3_FRAME
	stp	x0, x1, [sp, #0]
	b	el3_common
.endm

.section .text.vectors
.balign 0x800
.global el3_vector_table
el3_vector_table:
	/* Group 0: Current EL with SP_EL0 */
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	/* Group 1: Current EL with SP_ELx */
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	/* Group 2: Lower EL, AArch64 (SMC arrives at the sync slot) */
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	/* Group 3: Lower EL, AArch32 (unused) */
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY
	EL3_ENTRY

.section .text
.global el3_common
el3_common:
	stp	x2,  x3,  [sp, #16]
	stp	x4,  x5,  [sp, #32]
	stp	x6,  x7,  [sp, #48]
	stp	x8,  x9,  [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30,      [sp, #240]

	mov	x0, sp			// frame pointer
	bl	el3_sync_handler

	ldp	x2,  x3,  [sp, #16]
	ldp	x4,  x5,  [sp, #32]
	ldp	x6,  x7,  [sp, #48]
	ldp	x8,  x9,  [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30,      [sp, #240]
	ldp	x0,  x1,  [sp, #0]
	add	sp, sp, #EL3_FRAME
	eret
