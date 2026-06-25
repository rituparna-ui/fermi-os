// ARMv8-A exception vector table + common save/restore.
//
// Trap frame layout (must match TrapFrame in mod.rs):
//   [sp + 0]   x0 - x30      (31 * 8 = 248 bytes)
//   [sp + 248] ELR_EL1
//   [sp + 256] SPSR_EL1
//   [sp + 264] ESR_EL1
//   [sp + 272] FAR_EL1
//   [sp + 280] SP_EL0        (saved on EL0 exceptions; NOT a TrapFrame field —
//                             accessed via raw offset by the user/syscall code)
//   [sp + 288] q0
//   ...
//   [sp + 400] q7            (q0-q7: caller-clobbered FP/SIMD)
//   [sp + 416] q16
//   ...
//   [sp + 656] q31           (q16-q31: caller-clobbered FP/SIMD)
//   [sp + 672] FPSR
//   [sp + 680] FPCR
//   Total: 688 bytes, 16-byte aligned.
//
// q8-q15 are callee-saved per AAPCS64 and preserved by context_switch instead.
// We save q0-q7/q16-q31 because the handler chain (e.g. core::fmt) uses SIMD
// and would otherwise clobber a preempted EL0 task's caller-saved FP state.

.equ FRAME_SIZE, 688

.equ EXCEPTION_SYNC,    0
.equ EXCEPTION_IRQ,     1
.equ EXCEPTION_FIQ,     2
.equ EXCEPTION_SERROR,  3

// Vector entry stub (current EL, SP_ELx).
.macro VECTOR_ENTRY type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mov x0, #\type
	b exception_common
.endm

// Vector entry from a lower EL (EL0): also stash SP_EL0.
.macro VECTOR_ENTRY_LOWER type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mrs x0, sp_el0
	str x0, [sp, #280]
	mov x0, #\type
	b exception_common
.endm

// Invalid entry (should never fire).
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

// Group 1: Current EL with SP_ELx (kernel -> kernel)
	VECTOR_ENTRY EXCEPTION_SYNC
	VECTOR_ENTRY EXCEPTION_IRQ
	VECTOR_ENTRY EXCEPTION_FIQ
	VECTOR_ENTRY EXCEPTION_SERROR

// Group 2: Lower EL, AArch64 (user EL0 -> kernel EL1)
	VECTOR_ENTRY_LOWER EXCEPTION_SYNC
	VECTOR_ENTRY_LOWER EXCEPTION_IRQ
	VECTOR_ENTRY_LOWER EXCEPTION_FIQ
	VECTOR_ENTRY_LOWER EXCEPTION_SERROR

// Group 3: Lower EL, AArch32 (unused)
	VECTOR_ENTRY_INVALID EXCEPTION_SYNC
	VECTOR_ENTRY_INVALID EXCEPTION_IRQ
	VECTOR_ENTRY_INVALID EXCEPTION_FIQ
	VECTOR_ENTRY_INVALID EXCEPTION_SERROR

// Common handler. Entry: x0 = exception type; x0/x1 already saved at [sp].
.section .text
.global exception_common
exception_common:
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

	// System registers.
	mrs x1, elr_el1
	str x1, [sp, #248]
	mrs x1, spsr_el1
	str x1, [sp, #256]
	mrs x1, esr_el1
	str x1, [sp, #264]
	mrs x1, far_el1
	str x1, [sp, #272]

	// Caller-clobbered FP/SIMD (q0-q7, q16-q31, FPSR, FPCR).
	stp q0,  q1,  [sp, #288]
	stp q2,  q3,  [sp, #320]
	stp q4,  q5,  [sp, #352]
	stp q6,  q7,  [sp, #384]
	stp q16, q17, [sp, #416]
	stp q18, q19, [sp, #448]
	stp q20, q21, [sp, #480]
	stp q22, q23, [sp, #512]
	stp q24, q25, [sp, #544]
	stp q26, q27, [sp, #576]
	stp q28, q29, [sp, #608]
	stp q30, q31, [sp, #640]
	mrs x1, fpsr
	str x1, [sp, #672]
	mrs x1, fpcr
	str x1, [sp, #680]

	// exception_dispatch(type, frame)
	mov x1, sp
	bl exception_dispatch

	// Restore system registers.
	ldr x1, [sp, #248]
	msr elr_el1, x1
	ldr x1, [sp, #256]
	msr spsr_el1, x1

	// Restore SP_EL0 if returning to EL0 (SPSR_EL1.M[3:0] == 0).
	and x2, x1, #0xF
	cbnz x2, 1f
	ldr x1, [sp, #280]
	msr sp_el0, x1
1:

	// Restore FP/SIMD.
	ldr x1, [sp, #680]
	msr fpcr, x1
	ldr x1, [sp, #672]
	msr fpsr, x1
	ldp q30, q31, [sp, #640]
	ldp q28, q29, [sp, #608]
	ldp q26, q27, [sp, #576]
	ldp q24, q25, [sp, #544]
	ldp q22, q23, [sp, #512]
	ldp q20, q21, [sp, #480]
	ldp q18, q19, [sp, #448]
	ldp q16, q17, [sp, #416]
	ldp q6,  q7,  [sp, #384]
	ldp q4,  q5,  [sp, #352]
	ldp q2,  q3,  [sp, #320]
	ldp q0,  q1,  [sp, #288]

	// Restore general-purpose registers.
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

	add sp, sp, #FRAME_SIZE
	eret
