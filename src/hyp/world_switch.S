// FermiOS VHE hypervisor — world switch (milestone 3).
//
// vcpu_enter(vcpu_t *v)  [x0 = v]  enters the guest at EL1 and RETURNS to the
// host C caller when the guest traps to EL2 (KVM __guest_enter/__guest_exit
// pattern). This is NOT the M2 inline-eret path — the guest is a schedulable
// entity, so control comes back to C.
//
// VHE correctness invariants (verified):
//   * Guest HCR_EL2 = E2H|RW|VM only. We deliberately DO NOT set IMO/FMO/AMO,
//     so the host's EL1-physical-timer tick (PPI 30) is never misrouted to EL2
//     during the guest slice.
//   * TGE flips 1->0 on entry and is restored to the host value (TGE=1, VM=0)
//     on exit, entirely inside this asm with DAIF fully masked across the
//     VBAR/HCR swap windows, BEFORE any host C runs again.
//   * SP_EL2 is banked and preserved across the guest run, so the host frame we
//     build on it is intact on exit; we also stash host SP in the vcpu.
//   * Single guest => the guest owns the real EL1 sysreg bank; the host (EL2&0
//     under VHE) never uses it, so no _EL12 save/restore is needed here.
//
// vcpu_t field offsets — MUST match src/hyp/vcpu.h.
.equ VCPU_X0,          0      // x[0]; x[n] at 8*n; x[30] at 240
.equ VCPU_PC,          248    // ELR_EL2
.equ VCPU_PSTATE,      256    // SPSR_EL2
.equ VCPU_SP_EL1,      264
.equ VCPU_VTTBR,       272
.equ VCPU_HOST_SP,     280
.equ VCPU_HOST_X30,    288
.equ VCPU_EXIT_REASON, 296
.equ VCPU_ESR,         304
.equ VCPU_FAR,         312
.equ VCPU_HPFAR,       320

// Host frame built on the EL2 stack by vcpu_enter (160 bytes, 16-aligned):
//   [sp+0]  x19,x20  [sp+16] x21,x22  [sp+32] x23,x24  [sp+48] x25,x26
//   [sp+64] x27,x28  [sp+80] x29,x30  [sp+96] VBAR_EL2  [sp+104] HCR_EL2
//   [sp+112] DAIF
.equ HOST_FRAME, 160

.equ HYP_EXC_SYNC,   0
.equ HYP_EXC_IRQ,    1
.equ HYP_EXC_FIQ,    2
.equ HYP_EXC_SERROR, 3

.section .text
.global vcpu_enter
vcpu_enter:
	// --- save host context on the EL2 stack ---
	stp	x19, x20, [sp, #-HOST_FRAME]!
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]
	mrs	x1, vbar_el2
	str	x1, [sp, #96]
	mrs	x1, hcr_el2
	str	x1, [sp, #104]
	mrs	x1, daif
	str	x1, [sp, #112]

	// Mask all of DAIF for the whole switch + VBAR/HCR window.
	msr	daifset, #0xf
	isb

	// Stash vcpu pointer (x0) so the exit path can find it without a stack.
	msr	tpidr_el2, x0
	// Record host SP for the exit restore.
	mov	x1, sp
	str	x1, [x0, #VCPU_HOST_SP]

	// Install the guest-exit vector table.
	ldr	x1, =guest_vector_table
	msr	vbar_el2, x1
	isb

	// Program the stage-2 translation control. Fixed geometry for every guest:
	// VTCR_EL2 = 0x80023558 (T0SZ=24/40-bit IPA, SL0=1/start L1, WBWA, IS, 4K,
	// PS=40b, VS=8-bit VMID, RES1 bit31). Must be set before VTTBR/VM take
	// effect — leaving it at reset (0) makes every stage-2 walk fault.
	movz	x1, #0x3558
	movk	x1, #0x8002, lsl #16	// x1 = 0x80023558
	msr	vtcr_el2, x1
	isb

	// Program this guest's stage-2 base and flush stale stage-1+2 TLB.
	ldr	x1, [x0, #VCPU_VTTBR]
	msr	vttbr_el2, x1
	isb
	tlbi	vmalls12e1is
	dsb	ish
	isb

	// Guest HCR_EL2 = E2H(34) | RW(31) | VM(0). TGE=0, no IMO/FMO/AMO.
	mov	x2, #1
	lsl	x2, x2, #34		// E2H
	mov	x3, #1
	orr	x2, x2, x3, lsl #31	// RW
	orr	x2, x2, #1		// VM
	msr	hcr_el2, x2
	isb

	// Guest return state.
	ldr	x1, [x0, #VCPU_SP_EL1]
	msr	sp_el1, x1
	ldr	x1, [x0, #VCPU_PC]
	msr	elr_el2, x1
	ldr	x1, [x0, #VCPU_PSTATE]
	msr	spsr_el2, x1

	// Restore guest GPRs (x0 base, load x0 last).
	ldp	x2,  x3,  [x0, #16]
	ldp	x4,  x5,  [x0, #32]
	ldp	x6,  x7,  [x0, #48]
	ldp	x8,  x9,  [x0, #64]
	ldp	x10, x11, [x0, #80]
	ldp	x12, x13, [x0, #96]
	ldp	x14, x15, [x0, #112]
	ldp	x16, x17, [x0, #128]
	ldp	x18, x19, [x0, #144]
	ldp	x20, x21, [x0, #160]
	ldp	x22, x23, [x0, #176]
	ldp	x24, x25, [x0, #192]
	ldp	x26, x27, [x0, #208]
	ldp	x28, x29, [x0, #224]
	ldr	x30,      [x0, #240]
	ldr	x1,       [x0, #8]
	ldr	x0,       [x0, #0]
	eret

// ---------------------------------------------------------------------------
// Guest-exit vector table. Only the lower-EL-AArch64 group (the guest at EL1)
// is a real exit; current-EL and AArch32 groups indicate a hypervisor bug.
.macro GEXIT type
	.balign 0x80
	sub	sp, sp, #16
	stp	x0, x1, [sp]		// stash guest x0, x1
	mov	x1, #\type		// exit type (guest x1 already stashed)
	b	guest_exit_common
.endm

.macro GBAD
	.balign 0x80
	b	hyp_ws_bad
.endm

.section .text.hyp.gvectors
.balign 2048
.global guest_vector_table
guest_vector_table:
	// Group 0: Current EL, SP_EL0 (unused)
	GBAD
	GBAD
	GBAD
	GBAD
	// Group 1: Current EL, SP_ELx (hypervisor-internal fault — bug)
	GBAD
	GBAD
	GBAD
	GBAD
	// Group 2: Lower EL, AArch64 (guest exit — the hot path)
	GEXIT HYP_EXC_SYNC
	GEXIT HYP_EXC_IRQ
	GEXIT HYP_EXC_FIQ
	GEXIT HYP_EXC_SERROR
	// Group 3: Lower EL, AArch32 (guest is AArch64 — unused)
	GBAD
	GBAD
	GBAD
	GBAD

.section .text
guest_exit_common:
	// x1 = exit type; guest x0,x1 stashed at [sp]; other guest GPRs live.
	mrs	x0, tpidr_el2		// x0 = vcpu*
	str	x1, [x0, #VCPU_EXIT_REASON]
	stp	x2,  x3,  [x0, #16]
	stp	x4,  x5,  [x0, #32]
	stp	x6,  x7,  [x0, #48]
	stp	x8,  x9,  [x0, #64]
	stp	x10, x11, [x0, #80]
	stp	x12, x13, [x0, #96]
	stp	x14, x15, [x0, #112]
	stp	x16, x17, [x0, #128]
	stp	x18, x19, [x0, #144]
	stp	x20, x21, [x0, #160]
	stp	x22, x23, [x0, #176]
	stp	x24, x25, [x0, #192]
	stp	x26, x27, [x0, #208]
	stp	x28, x29, [x0, #224]
	str	x30,      [x0, #240]
	// recover stashed guest x0,x1 and store them
	ldp	x2, x3, [sp]
	str	x2, [x0, #0]
	str	x3, [x0, #8]
	add	sp, sp, #16

	// Guest return + syndrome state.
	mrs	x1, elr_el2
	str	x1, [x0, #VCPU_PC]
	mrs	x1, spsr_el2
	str	x1, [x0, #VCPU_PSTATE]
	mrs	x1, sp_el1
	str	x1, [x0, #VCPU_SP_EL1]
	mrs	x1, esr_el2
	str	x1, [x0, #VCPU_ESR]
	mrs	x1, far_el2
	str	x1, [x0, #VCPU_FAR]
	mrs	x1, hpfar_el2
	str	x1, [x0, #VCPU_HPFAR]

	// --- restore host context ---
	ldr	x1, [x0, #VCPU_HOST_SP]
	mov	sp, x1
	ldr	x1, [sp, #96]
	msr	vbar_el2, x1		// host vector table
	ldr	x1, [sp, #104]
	msr	hcr_el2, x1		// host HCR (TGE=1, VM=0)
	isb
	ldr	x1, [sp, #112]
	msr	daif, x1		// host interrupt mask
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	ldp	x19, x20, [sp], #HOST_FRAME
	ret

// Should-never-happen EL2 exception during the guest window. Spin loudly-ish.
hyp_ws_bad:
	wfe
	b	hyp_ws_bad
