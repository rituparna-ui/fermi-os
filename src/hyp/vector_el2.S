/* ---------------------------------------------------------------------------
 * vector_el2.S — EL2 exception vector table (Milestone 1)
 *
 * Standard ARMv8-A 16-entry layout (4 groups x 4 kinds, 0x80 bytes each,
 * table 0x800-aligned). Guest (EL1) traps land in group 2 ("lower EL,
 * AArch64"): HVC and stage-2 aborts arrive at the sync slot (index 8).
 *
 * CRITICAL: each vector slot is only 0x80 (128) bytes. The full register
 * save is therefore NOT inlined per slot (that overflows the slot and
 * corrupts the 0x80 spacing). Each stub does the minimum — bump SP, stash
 * x0/x1, record the slot index — then branches to el2_common.
 *
 * EL2 runs with its MMU off; the handler avoids FP so we need not manage
 * CPTR_EL2 / SIMD save state here.
 * --------------------------------------------------------------------------- */

.equ EL2_FRAME, 256

.macro EL2_ENTRY idx
.balign 0x80
	sub	sp, sp, #EL2_FRAME
	stp	x0, x1, [sp, #0]
	mov	x0, #\idx
	b	el2_common
.endm

.section .text.vectors
.balign 0x800
.global el2_vector_table
el2_vector_table:
	/* Group 0: Current EL with SP_EL0 */
	EL2_ENTRY 0
	EL2_ENTRY 1
	EL2_ENTRY 2
	EL2_ENTRY 3
	/* Group 1: Current EL with SP_ELx */
	EL2_ENTRY 4
	EL2_ENTRY 5
	EL2_ENTRY 6
	EL2_ENTRY 7
	/* Group 2: Lower EL, AArch64 (guest EL1 -> EL2). 8 = sync (HVC/abort) */
	EL2_ENTRY 8
	EL2_ENTRY 9
	EL2_ENTRY 10
	EL2_ENTRY 11
	/* Group 3: Lower EL, AArch32 (unused) */
	EL2_ENTRY 12
	EL2_ENTRY 13
	EL2_ENTRY 14
	EL2_ENTRY 15

/* Shared continuation: save x2-x30, dispatch, restore, eret.
 * On entry: x0 = vector index, x1 = scratch (already saved), x0/x1 at [sp]. */
.section .text
.global el2_common
el2_common:
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

	/* x0 already = vector index; pass frame pointer in x1. */
	mov	x1, sp
	bl	el2_dispatch

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
	add	sp, sp, #EL2_FRAME
	eret
