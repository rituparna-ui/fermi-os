// FermiOS EL2 hypervisor — dedicated EL2 exception vectors (milestone 2).
//
// Layout mirrors the host's src/exception/vector.S but reads the EL2-banked
// syndrome/return registers (ELR_EL2/SPSR_EL2/ESR_EL2/HPFAR_EL2/FAR_EL2) and
// builds its frame on the current (EL2) stack. The lower-EL-AArch64 group
// (entries 8-11) is the future guest-trap hot path; the current-EL groups
// catch hypervisor-internal faults; AArch32 / SP_EL0 groups are unused stubs.
//
// HYP_FRAME_SIZE = 288 — MUST match hyp_trap_frame_t in hyp.h:
//   [sp+0]   x0..x30   (248)
//   [sp+248] ELR_EL2   [sp+256] SPSR_EL2  [sp+264] ESR_EL2
//   [sp+272] HPFAR_EL2 [sp+280] FAR_EL2

.equ HYP_FRAME_SIZE, 288

.equ HYP_EXC_SYNC,   0
.equ HYP_EXC_IRQ,    1
.equ HYP_EXC_FIQ,    2
.equ HYP_EXC_SERROR, 3

// One vector entry: stash x0/x1, set x0 = exception type, branch to common.
.macro HYP_VEC type
	.balign 128
	sub	sp, sp, #HYP_FRAME_SIZE
	stp	x0, x1, [sp, #0]
	mov	x0, #\type
	b	hyp_exception_common
.endm

.section .text.hyp.vectors
.balign 2048
.global hyp_vector_table
hyp_vector_table:
	// Group 0: Current EL with SP_EL0 (unused at EL2h)
	HYP_VEC HYP_EXC_SYNC
	HYP_VEC HYP_EXC_IRQ
	HYP_VEC HYP_EXC_FIQ
	HYP_VEC HYP_EXC_SERROR

	// Group 1: Current EL with SP_ELx (hypervisor-internal exceptions)
	HYP_VEC HYP_EXC_SYNC
	HYP_VEC HYP_EXC_IRQ
	HYP_VEC HYP_EXC_FIQ
	HYP_VEC HYP_EXC_SERROR

	// Group 2: Lower EL, AArch64 (guest at EL1/EL0 — the hot path)
	HYP_VEC HYP_EXC_SYNC
	HYP_VEC HYP_EXC_IRQ
	HYP_VEC HYP_EXC_FIQ
	HYP_VEC HYP_EXC_SERROR

	// Group 3: Lower EL, AArch32 (guest is AArch64 — unused)
	HYP_VEC HYP_EXC_SYNC
	HYP_VEC HYP_EXC_IRQ
	HYP_VEC HYP_EXC_FIQ
	HYP_VEC HYP_EXC_SERROR

.section .text
.global hyp_exception_common
hyp_exception_common:
	// x0 = type, x0/x1 already saved at [sp,#0].
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

	// EL2-banked syndrome / return registers.
	mrs	x1, elr_el2
	str	x1, [sp, #248]
	mrs	x1, spsr_el2
	str	x1, [sp, #256]
	mrs	x1, esr_el2
	str	x1, [sp, #264]
	mrs	x1, hpfar_el2
	str	x1, [sp, #272]
	mrs	x1, far_el2
	str	x1, [sp, #280]

	// hyp_dispatch(type, frame)
	mov	x1, sp
	bl	hyp_dispatch

	// The dispatcher may have advanced ELR_EL2 or written emulated results
	// into the saved GPRs; restore everything from the (possibly mutated)
	// frame.
	ldr	x1, [sp, #248]
	msr	elr_el2, x1
	ldr	x1, [sp, #256]
	msr	spsr_el2, x1

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
	ldp	x0, x1, [sp, #0]

	add	sp, sp, #HYP_FRAME_SIZE
	eret
