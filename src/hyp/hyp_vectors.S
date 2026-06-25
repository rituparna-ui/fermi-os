// FermiOS hypervisor — EL2 exception vector table (Milestone 1).
//
// At M1 the guest runs with stage-2 OFF and nothing trapped to EL2, so in
// normal operation NONE of these vectors should fire. Each entry therefore
// records its index and jumps to a common stub that prints the EL2 fault
// state (ESR_EL2 / ELR_EL2 / FAR_EL2) and panics. Real handlers (stage-2
// MMIO emulation, HVC/PSCI, vGIC maintenance, world switch) replace the
// lower-EL groups in later milestones.
//
// VBAR_EL2 must be 2 KiB aligned; each of the 16 entries is 128 bytes apart.

.section .text.hyp.vectors
.balign 2048
.global hyp_vector_table
hyp_vector_table:

	// --- Current EL with SP_EL0 (EL2t) ---
	.balign 128
	mov	x0, #0
	b	hyp_vector_common
	.balign 128
	mov	x0, #1
	b	hyp_vector_common
	.balign 128
	mov	x0, #2
	b	hyp_vector_common
	.balign 128
	mov	x0, #3
	b	hyp_vector_common

	// --- Current EL with SP_ELx (EL2h) ---
	.balign 128
	mov	x0, #4
	b	hyp_vector_common
	.balign 128
	mov	x0, #5
	b	hyp_vector_common
	.balign 128
	mov	x0, #6
	b	hyp_vector_common
	.balign 128
	mov	x0, #7
	b	hyp_vector_common

	// --- Lower EL, AArch64 (guest at EL1/EL0) ---
	.balign 128
	mov	x0, #8			// sync (HVC, stage-2 abort, trapped sysreg)
	b	hyp_vector_common
	.balign 128
	mov	x0, #9			// IRQ
	b	hyp_vector_common
	.balign 128
	mov	x0, #10			// FIQ
	b	hyp_vector_common
	.balign 128
	mov	x0, #11			// SError
	b	hyp_vector_common

	// --- Lower EL, AArch32 (unused) ---
	.balign 128
	mov	x0, #12
	b	hyp_vector_common
	.balign 128
	mov	x0, #13
	b	hyp_vector_common
	.balign 128
	mov	x0, #14
	b	hyp_vector_common
	.balign 128
	mov	x0, #15
	b	hyp_vector_common

// Common stub: x0 = vector index. Report EL2 syndrome and panic.
.section .text
.global hyp_vector_common
hyp_vector_common:
	mrs	x1, esr_el2
	mrs	x2, elr_el2
	mrs	x3, far_el2
	// Hand off to C reporter (it does not return).
	bl	hyp_vector_report
hvc_hang:
	wfe
	b	hvc_hang
