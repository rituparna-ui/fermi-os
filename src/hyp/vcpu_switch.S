// Per-vCPU EL1 sysreg + FP context save/restore for the EL2 scheduler.
//
// The C code is built -mgeneral-regs-only (so the GPR-only trap frame is safe),
// but THIS hand-written assembly may freely use q-registers to save/restore the
// guest's FP state — the compiler restriction does not apply to .S files.
//
// vcpu_save_sysregs(vcpu_sysregs_t *s)   — x0 = s
// vcpu_restore_sysregs(const vcpu_sysregs_t *s)
// vcpu_save_fp(vcpu_fp_t *f)             — x0 = f
// vcpu_restore_fp(const vcpu_fp_t *f)
//
// vcpu_sysregs_t field offsets (must match vcpu.h, 8 bytes each):
//   0 sctlr_el1   8 cpacr_el1   16 ttbr0_el1   24 ttbr1_el1   32 tcr_el1
//  40 mair_el1   48 amair_el1   56 vbar_el1    64 contextidr_el1
//  72 tpidr_el0  80 tpidrro_el0 88 tpidr_el1   96 sp_el0     104 sp_el1
// 112 elr_el1   120 spsr_el1   128 esr_el1    136 far_el1    144 par_el1

.section .text

// vcpu_first_entry(vcpu_gp_t *gp) — x0 = gp. Loads ELR_EL2/SPSR_EL2 and the
// guest GPRs, then eret. Does not return. vcpu_gp_t layout: x[0..30] at offset
// 0 (248 bytes), elr_el2 @ 248, spsr_el2 @ 256.
.global vcpu_first_entry
vcpu_first_entry:
	ldr	x1, [x0, #248] ; msr elr_el2, x1
	ldr	x1, [x0, #256] ; msr spsr_el2, x1
	// Restore x2..x30 first, then x0/x1 last (x0 holds gp until the end).
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
	ldp	x0,  x1,  [x0, #0]
	eret

.global vcpu_save_sysregs
vcpu_save_sysregs:
	mrs	x1, sctlr_el1   ; str x1, [x0, #0]
	mrs	x1, cpacr_el1   ; str x1, [x0, #8]
	mrs	x1, ttbr0_el1   ; str x1, [x0, #16]
	mrs	x1, ttbr1_el1   ; str x1, [x0, #24]
	mrs	x1, tcr_el1     ; str x1, [x0, #32]
	mrs	x1, mair_el1    ; str x1, [x0, #40]
	mrs	x1, amair_el1   ; str x1, [x0, #48]
	mrs	x1, vbar_el1    ; str x1, [x0, #56]
	mrs	x1, contextidr_el1 ; str x1, [x0, #64]
	mrs	x1, tpidr_el0   ; str x1, [x0, #72]
	mrs	x1, tpidrro_el0 ; str x1, [x0, #80]
	mrs	x1, tpidr_el1   ; str x1, [x0, #88]
	mrs	x1, sp_el0      ; str x1, [x0, #96]
	mrs	x1, sp_el1      ; str x1, [x0, #104]
	mrs	x1, elr_el1     ; str x1, [x0, #112]
	mrs	x1, spsr_el1    ; str x1, [x0, #120]
	mrs	x1, esr_el1     ; str x1, [x0, #128]
	mrs	x1, far_el1     ; str x1, [x0, #136]
	mrs	x1, par_el1     ; str x1, [x0, #144]
	ret

.global vcpu_restore_sysregs
vcpu_restore_sysregs:
	ldr	x1, [x0, #0]   ; msr sctlr_el1, x1
	ldr	x1, [x0, #8]   ; msr cpacr_el1, x1
	ldr	x1, [x0, #16]  ; msr ttbr0_el1, x1
	ldr	x1, [x0, #24]  ; msr ttbr1_el1, x1
	ldr	x1, [x0, #32]  ; msr tcr_el1, x1
	ldr	x1, [x0, #40]  ; msr mair_el1, x1
	ldr	x1, [x0, #48]  ; msr amair_el1, x1
	ldr	x1, [x0, #56]  ; msr vbar_el1, x1
	ldr	x1, [x0, #64]  ; msr contextidr_el1, x1
	ldr	x1, [x0, #72]  ; msr tpidr_el0, x1
	ldr	x1, [x0, #80]  ; msr tpidrro_el0, x1
	ldr	x1, [x0, #88]  ; msr tpidr_el1, x1
	ldr	x1, [x0, #96]  ; msr sp_el0, x1
	ldr	x1, [x0, #104] ; msr sp_el1, x1
	ldr	x1, [x0, #112] ; msr elr_el1, x1
	ldr	x1, [x0, #120] ; msr spsr_el1, x1
	ldr	x1, [x0, #128] ; msr esr_el1, x1
	ldr	x1, [x0, #136] ; msr far_el1, x1
	ldr	x1, [x0, #144] ; msr par_el1, x1
	// Sysreg writes that affect translation/exception state need an ISB
	// before the guest runs again.
	isb
	ret

// vcpu_fp_t: q[0..31] at offset 0 (512 bytes), fpsr @ 512, fpcr @ 520.
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
