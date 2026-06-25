// Kernel entry point — higher-half boot.
//
// The kernel is linked at upper-half VAs (0xFFFF_0000_4000_0000) but loaded and
// entered physically at 0x4000_0000 with the MMU off. So `ldr =sym` literal
// loads yield VAs that we must convert to PAs (subtract KERNEL_VA_OFFSET) until
// the MMU is enabled. `early_init` builds the page tables and turns the MMU on;
// after it returns we reload SP with the upper-half VA and branch to the
// upper-half kmain.

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000
.equ EL2_STACK_SIZE,   8192
.equ SPSR_EL1H_DAIF,   0x3c5	// M=EL1h(0b0101), DAIF masked(0b1111<<6)

.section .text.boot
.global _start

_start:
	// Park all secondary CPUs: only the primary (MPIDR Aff0 == 0) proceeds.
	// MPIDR_EL1 is readable at both EL1 and EL2, so this is valid either way.
	mrs		x1, mpidr_el1
	and		x1, x1, #0xff
	cbnz	x1, halt

	// Physical stack top: symbol is a VA, subtract the offset (MMU is off).
	// This sets SP for whichever EL we entered at (SP_EL2 if EL2, SP_EL1 if
	// EL1) — both share the encoding `mov sp, xN` for the current EL's SP.
	ldr		x0, =__stack_top
	ldr		x1, =KERNEL_VA_OFFSET
	sub		x0, x0, x1
	mov		sp, x0

	// With `virt,virtualization=on` QEMU enters the image at EL2. Detect it: if
	// we're at EL2, bring up the hypervisor and eret down to EL1. If we entered
	// at EL1 directly (virtualization off), skip straight to the kernel path —
	// fully backwards compatible.
	mrs		x0, CurrentEL
	lsr		x0, x0, #2
	cmp		x0, #2
	b.ne	el1_entry			// not EL2 -> already EL1, run kernel directly

	// --- EL2: configure stage-2, vtimer, VBAR_EL2, HCR_EL2.VM ---
	// hyp_init runs on the current (boot) stack; nothing else is live yet. It
	// is PC-relative (bl), so it reaches physical hyp_init with the MMU off.
	bl		hyp_init

	// Repoint SP_EL2 at a dedicated EL2 stack so future guest->EL2 traps don't
	// clobber the EL1 kernel stack (which SP_EL1 keeps using).
	ldr		x0, =el2_stack
	ldr		x1, =KERNEL_VA_OFFSET
	sub		x0, x0, x1			// physical base of el2_stack
	add		x0, x0, #EL2_STACK_SIZE	// top of EL2 stack
	mov		sp, x0

	// Prepare the drop to EL1: resume at el1_entry (physical, PC-relative), in
	// EL1h with interrupts masked, with SP_EL1 set to the kernel stack.
	adr		x0, el1_entry
	msr		elr_el2, x0
	mov		x0, #SPSR_EL1H_DAIF
	msr		spsr_el2, x0
	ldr		x0, =__stack_top
	ldr		x1, =KERNEL_VA_OFFSET
	sub		x0, x0, x1
	msr		sp_el1, x0
	eret

el1_entry:
	// Running at EL1 now (natively, or via the eret above). Re-establish the
	// physical kernel stack, then run the existing boot sequence.
	ldr		x0, =__stack_top
	ldr		x1, =KERNEL_VA_OFFSET
	sub		x0, x0, x1
	mov		sp, x0

	// Zero .bss using physical addresses. QEMU's -kernel loader does not clear
	// it, and the kernel's statics (locks, allocators) rely on zero init.
	ldr		x0, =__bss_start
	ldr		x2, =KERNEL_VA_OFFSET
	sub		x0, x0, x2			// physical __bss_start
	ldr		x1, =__bss_end
	sub		x1, x1, x2			// physical __bss_end
zero_bss:
	cmp		x0, x1
	b.hs	bss_done
	str		xzr, [x0], #8
	b		zero_bss
bss_done:

	// early_init runs physically (PC-relative bl). It enables the MMU so that
	// TTBR1 maps the upper half and TTBR0 identity-maps the lower half.
	bl		early_init

	// MMU is on. Switch to the upper-half stack and branch to the upper-half
	// kmain via an absolute (literal-pool) address.
	ldr		x0, =__stack_top
	mov		sp, x0

	ldr		x1, =kmain
	br		x1

	// kmain should never return; halt if it does.
halt:
	wfe
	b		halt
