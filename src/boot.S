.section .text.boot
.global _start

_start:
	ldr x0, =__stack_top
	mov sp, x0

	/* Zero out BSS section */
	ldr x0, =__bss_start
	ldr x1, =__bss_end
1:
	cmp x0, x1
	b.ge 2f
	str xzr, [x0], #8
	b 1b
2:

	bl kernel_main
	/* If kernel_main returns, hang */
kernel_panic:
	wfe
	b kernel_panic
