.section .text.boot
.global _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000

_start:
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
