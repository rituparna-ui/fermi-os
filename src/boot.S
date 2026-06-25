.section .text.boot
.global _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000

_start:
	// --- Exception-level dispatch -------------------------------------
	// QEMU `virt,virtualization=on` enters -kernel at EL2. We turn the
	// kernel into a VHE hypervisor host: setting HCR_EL2.E2H makes every
	// _EL1 system-register accessor in the existing C code (SCTLR_EL1,
	// TCR_EL1, TTBR0/1_EL1, MAIR_EL1, VBAR_EL1, ELR/SPSR/ESR/FAR_EL1, the
	// *e1 TLBIs, CPACR_EL1) hardware-redirect to its EL2 bank. The rest of
	// boot is then byte-identical to the legacy EL1 path.
	//
	// If entered at EL1 (no virtualization=on, or this image running as a
	// guest), skip the preamble and boot exactly as before.
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	and	x0, x0, #3
	cmp	x0, #2
	b.ne	el1_phys_sp		// not EL2 -> legacy boot path

	// --- EL2: enter VHE host context ----------------------------------
	// HCR_EL2: RW[31]=1 (EL1 is AArch64), TGE[27]=1 (host EL0 / lower-EL
	// exceptions use the EL2&0 regime and route to EL2), E2H[34]=1 (the
	// _EL1 -> EL2 register redirect; E2H is THE switch, independent of TGE).
	mrs	x0, hcr_el2
	mov	x1, #1
	orr	x0, x0, x1, lsl #31	// RW
	orr	x0, x0, x1, lsl #27	// TGE
	orr	x0, x0, x1, lsl #34	// E2H
	msr	hcr_el2, x0
	isb				// MANDATORY before the first _EL1 access below

	// FP/SIMD: under E2H=1 CPTR_EL2 uses the CPACR format; FPEN[21:20]=0b11
	// disables FP/SIMD trapping (uart_printf passes varargs in SIMD regs).
	// (Do NOT touch the non-E2H TFP[10] bit — it is not in this layout.)
	mov	x0, #(3 << 20)
	msr	cptr_el2, x0
	isb

	// GICv3 system-register interface: enable ICC_SRE_EL2.{SRE[0],Enable[3]}
	// BEFORE gic.c writes ICC_SRE_EL1, otherwise that EL1 write is ignored
	// and the CPU interface never comes up.
	mrs	x0, icc_sre_el2
	orr	x0, x0, #(1 << 0)	// SRE: system-register interface
	orr	x0, x0, #(1 << 3)	// Enable: permit lower-EL ICC_SRE_EL1 access
	msr	icc_sre_el2, x0
	isb

	// Generic timer: let EL1 read the physical counter/timer without
	// trapping (prep for guests; harmless for the host, which runs at EL2
	// and is never gated by CNTHCTL_EL2). Under E2H=1 the controlling
	// fields are CNTHCTL_EL2.EL1PCTEN[10] (counter) and EL1PTEN[11] (timer).
	mrs	x0, cnthctl_el2
	orr	x0, x0, #(1 << 10)	// EL1PCTEN
	orr	x0, x0, #(1 << 11)	// EL1PTEN
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr	// zero virtual-counter offset
	isb

	// Fall through: early_init now executes in EL2 VHE host context.

el1_phys_sp:
	// Symbols are now linked at upper-half VAs.
	// Before MMU is on, we need PHYSICAL addresses, so subtract the offset.
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical stack top
	mov sp, x0

	// PC is physical. early_init is linked at upper-half but bl is PC-relative,
	// so it branches to physical early_init. early_init enables MMU.
	bl early_init

	// MMU now on, TTBR1 maps upper half. Switch to upper-half VAs directly.
	ldr x0, =__stack_top
	mov sp, x0

	ldr x1, =kernel_main
	br x1

	// panic if kernel_main returns
	ldr x1, =kernel_panic_return
	br x1
