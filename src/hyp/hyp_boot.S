// FermiOS type-1 hypervisor — EL2 reset entry (Milestone 1).
//
// QEMU '-kernel build/hyp.elf' on 'virt,gic-version=3,virtualization=on -cpu max'
// enters here at EL2 with the MMU off. We are linked+loaded at host physical
// 0x250000000 (above the guest's 8 GiB RAM), so every symbol address here is
// already a physical address — no VA offset math needed (unlike the guest's
// higher-half boot.S).
//
// Flow:
//   1. Confirm CurrentEL == EL2 (else panic).
//   2. Set up SP_EL2, VBAR_EL2, SCTLR_EL2 (MMU/caches off).
//   3. CPTR_EL2 = 0x32FF  -> do NOT trap guest FP/SIMD/SVE.
//   4. MDCR_EL2: no debug/PMU traps; HPMN = implemented PMU counter count.
//   5. VPIDR_EL2/VMPIDR_EL2 = real MIDR/MPIDR (so guest /proc/cpuinfo is sane).
//   6. CNTHCTL_EL2 = 0x3  -> guest physical timer passthrough.
//   7. CNTVOFF_EL2 = 0.
//   8. HCR_EL2 = RW (AArch64 guest; stage-2 OFF for M1).
//   9. Call hyp_main() (C, EL2, MMU off).
//  10. eret to the guest: ELR_EL2 = 0x40000000, SPSR_EL2 = EL1h+DAIF.
//
// We deliberately NEVER touch SCR_EL3 — there is no EL3 on QEMU 'virt' without
// secure=on, and an EL3 register access from EL2 would be UNDEFINED.

#include "hyp_sysregs.h"

.section .text.boot.hyp
.global _hyp_start
_hyp_start:
	// --- 1. Must be at EL2 ---
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	and	x0, x0, #3
	cmp	x0, #2
	b.eq	1f
	// Not EL2: print "[HYP] not EL2" via literal PL011 and spin. We cannot
	// call C safely without a stack, so poke the UART by hand here.
	ldr	x9, =0x09000000		// UART DR
	mov	w10, #'!'
	str	w10, [x9]
	mov	w10, #'E'
	str	w10, [x9]
	mov	w10, #'2'
	str	w10, [x9]
	mov	w10, #'\n'
	str	w10, [x9]
0:	wfe
	b	0b
1:

	// --- 2. SP_EL2. SPSel is already 1 (SP_EL2) at EL2 reset. ---
	ldr	x0, =__hyp_stack_top
	mov	sp, x0

	// VBAR_EL2 -> EL2 vector table.
	ldr	x0, =hyp_vector_table
	msr	vbar_el2, x0
	isb

	// SCTLR_EL2: MMU off, caches off, SA=1.
	ldr	x0, =SCTLR_EL2_RES1_OFF
	msr	sctlr_el2, x0
	isb

	// --- 3. CPTR_EL2 = 0x32FF: don't trap guest FP/SIMD (TFP=0) or SVE (TZ=0). ---
	ldr	x0, =CPTR_EL2_M1
	msr	cptr_el2, x0
	isb

	// --- 4. MDCR_EL2: no debug/PMU traps; HPMN = PMCR_EL0.N (implemented counters). ---
	mrs	x1, pmcr_el0
	ubfx	x1, x1, #11, #5		// PMCR_EL0.N (bits[15:11]) = number of event counters
	and	x1, x1, #MDCR_EL2_HPMN_MASK
	msr	mdcr_el2, x1		// all trap bits 0, HPMN in [4:0]
	isb

	// --- 5. VPIDR_EL2 / VMPIDR_EL2 = real IDs (guest reads MIDR/MPIDR through these). ---
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0

	// --- 6/7. Timer: passthrough physical timer to the guest; zero virtual offset. ---
	mov	x0, #CNTHCTL_EL2_M1	// 0x3 = EL1PCTEN | EL1PCEN
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	isb

	// --- 8. HCR_EL2 = RW (guest is AArch64; stage-2 off for M1, TGE=0, HCD=0). ---
	ldr	x0, =HCR_EL2_M1
	msr	hcr_el2, x0
	isb

	// Conventional hardening even with MMU off: drop stale EL2 TLB + I-cache.
	tlbi	alle2
	dsb	sy
	ic	iallu
	dsb	sy
	isb

	// --- 9. C entry at EL2. hyp_main sets up both VMs + the scheduler and
	// enters the first guest via vcpu_run_first(); it does NOT return. ---
	bl	hyp_main

	// hyp_main should never return; if it does, spin.
2:	wfe
	b	2b
