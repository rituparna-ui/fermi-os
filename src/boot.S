.section .text.boot
.global _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000
.equ EL2_STACK_SIZE,   8192
.equ SPSR_EL1H_DAIF,   0x3c5   // M=EL1h(0b0101), DAIF masked(0b1111<<6)

_start:
	// Symbols are linked at upper-half VAs. Before any MMU is on we need
	// PHYSICAL addresses, so subtract the offset. This sets the stack for
	// whichever EL we entered at (SP_EL2 if EL2, SP_EL1 if EL1).
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical stack top
	mov sp, x0

	// With `virt,virtualization=on` QEMU enters the image at EL2. Detect it:
	// if we're at EL2, bring up the hypervisor and eret down to EL1. If we
	// entered at EL1 directly (virtualization off), skip straight to the
	// kernel path — fully backwards compatible.
	mrs x0, CurrentEL
	lsr x0, x0, #2
	cmp x0, #2
	b.ne 1f				// not EL2 -> already EL1, run kernel directly

	// --- EL2: configure stage-2, vtimer, VBAR_EL2, HCR_EL2.VM ---
	// rmm_init runs on the current (boot) stack; nothing else is live yet.
	bl rmm_init

	// Repoint SP_EL2 at a dedicated EL2 stack so future guest->EL2 traps
	// don't clobber the EL1 kernel stack (which SP_EL1 keeps using).
	ldr x0, =el2_stack
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical base of el2_stack
	add x0, x0, #EL2_STACK_SIZE	// top of EL2 stack
	mov sp, x0

	// Prepare the drop to EL1: resume at label 1 (physical, PC-relative),
	// in EL1h with interrupts masked, with SP_EL1 set to the kernel stack.
	adr x0, 1f
	msr elr_el2, x0
	mov x0, #SPSR_EL1H_DAIF
	msr spsr_el2, x0
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	msr sp_el1, x0
	eret

1:
	// Running at EL1 now (natively, or via the eret above). Ensure SP points
	// at the physical kernel stack, then run the existing boot sequence.
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical stack top
	mov sp, x0

	// PC is physical. early_init is linked upper-half but bl is PC-relative,
	// so it branches to physical early_init. early_init enables the stage-1
	// MMU.
	bl early_init

	// MMU now on, TTBR1 maps upper half. Switch to upper-half VAs directly.
	ldr x0, =__stack_top
	mov sp, x0

	ldr x1, =kernel_main
	br x1

	// panic if kernel_main returns
	ldr x1, =kernel_panic_return
	br x1
