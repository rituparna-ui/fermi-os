.section .text.boot
.global _start

_start:
	ldr x0, =__stack_top
	mov sp, x0

	b kernel_main
	// TODO: add panic handler
	// bl kernel_main
	// b kernel_panic