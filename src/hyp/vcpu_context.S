// Per-vCPU EL1 sysreg + FP save/restore for the multi-guest scheduler (M9).
//
// Under VHE (HCR_EL2.E2H=1) the hypervisor runs at EL2 and its plain _EL1
// accessors hit the EL2 bank (the host's own state). To reach the GUEST's EL1
// bank from EL2 we use the _EL12 aliases — that is what these routines do. The
// host therefore never disturbs the guest EL1 bank; we only move it explicitly
// when switching between two guests.
//
// vcpu_el1_t field offsets (must match struct vcpu_el1 in vcpu.h; 8B each):
//   0 sctlr   8 cpacr  16 ttbr0  24 ttbr1  32 tcr   40 mair  48 amair  56 vbar
//  64 contextidr  72 tpidr_el0  80 tpidrro_el0  88 tpidr_el1
//  96 sp_el0  104 elr_el1  112 spsr_el1  120 esr_el1  128 far_el1  136 par_el1
// 144 cntv_cval  152 cntv_ctl  160 cntkctl

// The _EL12 / _EL02 register aliases are a VHE (ARMv8.1) feature; tell the
// assembler the target implements it so it accepts these mnemonics.
.arch armv8.1-a

.section .text

.global vcpu_save_el1
vcpu_save_el1:
	mrs	x1, sctlr_el12      ; str x1, [x0, #0]
	mrs	x1, cpacr_el12      ; str x1, [x0, #8]
	mrs	x1, ttbr0_el12      ; str x1, [x0, #16]
	mrs	x1, ttbr1_el12      ; str x1, [x0, #24]
	mrs	x1, tcr_el12        ; str x1, [x0, #32]
	mrs	x1, mair_el12       ; str x1, [x0, #40]
	mrs	x1, amair_el12      ; str x1, [x0, #48]
	mrs	x1, vbar_el12       ; str x1, [x0, #56]
	mrs	x1, contextidr_el12 ; str x1, [x0, #64]
	mrs	x1, tpidr_el0       ; str x1, [x0, #72]
	mrs	x1, tpidrro_el0     ; str x1, [x0, #80]
	mrs	x1, tpidr_el1       ; str x1, [x0, #88]
	mrs	x1, sp_el0          ; str x1, [x0, #96]
	mrs	x1, elr_el12        ; str x1, [x0, #104]
	mrs	x1, spsr_el12       ; str x1, [x0, #112]
	mrs	x1, esr_el12        ; str x1, [x0, #120]
	mrs	x1, far_el12        ; str x1, [x0, #128]
	mrs	x1, par_el1         ; str x1, [x0, #136]
	mrs	x1, cntv_cval_el02  ; str x1, [x0, #144]
	mrs	x1, cntv_ctl_el02   ; str x1, [x0, #152]
	mrs	x1, cntkctl_el12    ; str x1, [x0, #160]
	ret

.global vcpu_restore_el1
vcpu_restore_el1:
	ldr	x1, [x0, #0]   ; msr sctlr_el12, x1
	ldr	x1, [x0, #8]   ; msr cpacr_el12, x1
	ldr	x1, [x0, #16]  ; msr ttbr0_el12, x1
	ldr	x1, [x0, #24]  ; msr ttbr1_el12, x1
	ldr	x1, [x0, #32]  ; msr tcr_el12, x1
	ldr	x1, [x0, #40]  ; msr mair_el12, x1
	ldr	x1, [x0, #48]  ; msr amair_el12, x1
	ldr	x1, [x0, #56]  ; msr vbar_el12, x1
	ldr	x1, [x0, #64]  ; msr contextidr_el12, x1
	ldr	x1, [x0, #72]  ; msr tpidr_el0, x1
	ldr	x1, [x0, #80]  ; msr tpidrro_el0, x1
	ldr	x1, [x0, #88]  ; msr tpidr_el1, x1
	ldr	x1, [x0, #96]  ; msr sp_el0, x1
	ldr	x1, [x0, #104] ; msr elr_el12, x1
	ldr	x1, [x0, #112] ; msr spsr_el12, x1
	ldr	x1, [x0, #120] ; msr esr_el12, x1
	ldr	x1, [x0, #128] ; msr far_el12, x1
	ldr	x1, [x0, #136] ; msr par_el1, x1
	ldr	x1, [x0, #144] ; msr cntv_cval_el02, x1
	ldr	x1, [x0, #152] ; msr cntv_ctl_el02, x1
	ldr	x1, [x0, #160] ; msr cntkctl_el12, x1
	isb
	ret

// vcpu_fp_t: q[0..31] at offset 0 (512B), fpsr @ 512, fpcr @ 520.
.global vcpu_save_fp
vcpu_save_fp:
	stp	q0,  q1,  [x0, #0]
	stp	q2,  q3,  [x0, #32]
	stp	q4,  q5,  [x0, #64]
	stp	q6,  q7,  [x0, #96]
	stp	q8,  q9,  [x0, #128]
	stp	q10, q11, [x0, #160]
	stp	q12, q13, [x0, #192]
	stp	q14, q15, [x0, #224]
	stp	q16, q17, [x0, #256]
	stp	q18, q19, [x0, #288]
	stp	q20, q21, [x0, #320]
	stp	q22, q23, [x0, #352]
	stp	q24, q25, [x0, #384]
	stp	q26, q27, [x0, #416]
	stp	q28, q29, [x0, #448]
	stp	q30, q31, [x0, #480]
	mrs	x1, fpsr ; str x1, [x0, #512]
	mrs	x1, fpcr ; str x1, [x0, #520]
	ret

.global vcpu_restore_fp
vcpu_restore_fp:
	ldp	q0,  q1,  [x0, #0]
	ldp	q2,  q3,  [x0, #32]
	ldp	q4,  q5,  [x0, #64]
	ldp	q6,  q7,  [x0, #96]
	ldp	q8,  q9,  [x0, #128]
	ldp	q10, q11, [x0, #160]
	ldp	q12, q13, [x0, #192]
	ldp	q14, q15, [x0, #224]
	ldp	q16, q17, [x0, #256]
	ldp	q18, q19, [x0, #288]
	ldp	q20, q21, [x0, #320]
	ldp	q22, q23, [x0, #352]
	ldp	q24, q25, [x0, #384]
	ldp	q26, q27, [x0, #416]
	ldp	q28, q29, [x0, #448]
	ldp	q30, q31, [x0, #480]
	ldr	x1, [x0, #512] ; msr fpsr, x1
	ldr	x1, [x0, #520] ; msr fpcr, x1
	ret
