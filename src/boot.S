.section .text.boot
.global _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000

_start:
	ldr x0, =__stack_top
	mov sp, x0

	// runs at physical addresses
	// sets up UART, PMM, MMU
	bl early_init

	// jump to upper half
	// TTBR1 maps 0xFFFF_0000_0000_0000+ to PA 0x0000+
	// just add KERNEL_VA_OFFSET to jump address
	
	// relocate stack to upper half
	ldr x0, =KERNEL_VA_OFFSET
	add sp, sp, x0

	// upper half address of kernel_main
	// bl is a relative jump
	ldr x1, =kernel_main
	add x1, x1, x0
	br x1

	// panic if kernel_main returns
	ldr x0, =KERNEL_VA_OFFSET
	ldr x1, =kernel_panic_return
	add x1, x1, x0
	br x1
