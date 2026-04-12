// Trap frame layout:
//   [sp + 0]   x0 - x30    (31 × 8 = 248 bytes)
//   [sp + 248] ELR_EL1
//   [sp + 256] SPSR_EL1
//   [sp + 264] ESR_EL1
//   [sp + 272] FAR_EL1
//   [sp + 280] SP_EL0      (user stack pointer, saved on EL0 exceptions)
//   Total: 288 bytes → 288 aligned to 16

.equ FRAME_SIZE, 288

.equ EXCEPTION_SYNC,    0
.equ EXCEPTION_IRQ,     1
.equ EXCEPTION_FIQ,     2
.equ EXCEPTION_SERROR,  3

// Macro: vector entry stub (current EL, SP_ELx)
.macro VECTOR_ENTRY type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mov x0, #\type
	b exception_common
.endm

// Macro: vector entry from lower EL (EL0)
// Save SP_EL0 since we're now on the kernel stack
.macro VECTOR_ENTRY_LOWER type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mrs x0, sp_el0
	str x0, [sp, #280]
	mov x0, #\type
	b exception_common
.endm

// invalid vector entry (should never fire)
.macro VECTOR_ENTRY_INVALID type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mov x0, #\type
	b exception_common
.endm

.section .text.vectors
.balign 2048
.global vector_table
vector_table:
// Group 0: Current EL with SP_EL0 (unused)
	VECTOR_ENTRY_INVALID EXCEPTION_SYNC
	VECTOR_ENTRY_INVALID EXCEPTION_IRQ
	VECTOR_ENTRY_INVALID EXCEPTION_FIQ
	VECTOR_ENTRY_INVALID EXCEPTION_SERROR

// Group 1: Current EL with SP_ELx (kernel → kernel)
	VECTOR_ENTRY EXCEPTION_SYNC
	VECTOR_ENTRY EXCEPTION_IRQ
	VECTOR_ENTRY EXCEPTION_FIQ
	VECTOR_ENTRY EXCEPTION_SERROR

// Group 2: Lower EL, AArch64 (user EL0 → kernel EL1)
	VECTOR_ENTRY_LOWER EXCEPTION_SYNC
	VECTOR_ENTRY_LOWER EXCEPTION_IRQ
	VECTOR_ENTRY_LOWER EXCEPTION_FIQ
	VECTOR_ENTRY_LOWER EXCEPTION_SERROR

// Group 3: Lower EL, AArch32 (unused)
	VECTOR_ENTRY_INVALID EXCEPTION_SYNC
	VECTOR_ENTRY_INVALID EXCEPTION_IRQ
	VECTOR_ENTRY_INVALID EXCEPTION_FIQ
	VECTOR_ENTRY_INVALID EXCEPTION_SERROR

// Common exception handler
// Entry: x0 = exception type, x0/x1 saved at [sp]
.section .text
.global exception_common
exception_common:
	// x0 and x1 already saved at [sp, #0] by the vector entry
	// save remaining general-purpose registers
	stp x2,  x3,  [sp, #16]
	stp x4,  x5,  [sp, #32]
	stp x6,  x7,  [sp, #48]
	stp x8,  x9,  [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x19, [sp, #144]
	stp x20, x21, [sp, #160]
	stp x22, x23, [sp, #176]
	stp x24, x25, [sp, #192]
	stp x26, x27, [sp, #208]
	stp x28, x29, [sp, #224]
	str x30,      [sp, #240]

	// save system registers
	mrs x1, elr_el1
	str x1, [sp, #248]
	mrs x1, spsr_el1
	str x1, [sp, #256]
	mrs x1, esr_el1
	str x1, [sp, #264]
	mrs x1, far_el1
	str x1, [sp, #272]

	// call C handler: exception_dispatch(type, frame)
	// x0 = exception type (already set)
	mov x1, sp
	bl exception_dispatch

	// restore system registers
	ldr x1, [sp, #248]
	msr elr_el1, x1
	ldr x1, [sp, #256]
	msr spsr_el1, x1

	// Restore SP_EL0 if returning to EL0
	// Check SPSR_EL1.M[3:0] — if 0, we're returning to EL0
	and x2, x1, #0xF
	cbnz x2, 1f
	ldr x1, [sp, #280]
	msr sp_el0, x1
1:

	// restore general-purpose registers
	ldp x2,  x3,  [sp, #16]
	ldp x4,  x5,  [sp, #32]
	ldp x6,  x7,  [sp, #48]
	ldp x8,  x9,  [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x19, [sp, #144]
	ldp x20, x21, [sp, #160]
	ldp x22, x23, [sp, #176]
	ldp x24, x25, [sp, #192]
	ldp x26, x27, [sp, #208]
	ldp x28, x29, [sp, #224]
	ldr x30,      [sp, #240]
	ldp x0, x1, [sp, #0]

	// deallocate trap frame
	add sp, sp, #FRAME_SIZE

	eret
