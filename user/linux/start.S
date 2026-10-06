/*
 * _start for a static Linux aarch64 binary.
 *
 * This is the Linux process-entry contract: the kernel hands control over with
 * SP_EL0 pointing at argc, and nothing in the registers is defined. Everything
 * else -- argv, envp, the auxiliary vector -- is found by walking up the stack
 * from there.
 *
 * Fermi's own crt0 instead passes argc/argv in x0/x1. The kernel satisfies both
 * conventions at once (it builds the Linux stack *and* sets those registers), so
 * this binary and Fermi-linked binaries can share one exec path with nothing to
 * tell them apart.
 *
 * There is no libc here: this file is what a dynamically linked binary's
 * interpreter would otherwise do for it, minus the loading part.
 */
	.text
	.global _start
	.type _start, %function
_start:
	ldr	x19, [sp]		// argc
	add	x20, sp, #8		// &argv[0]
	// envp sits immediately after the NULL-terminated argv array.
	add	x21, x20, x19, lsl #3
	add	x21, x21, #8

	mov	x0, x19		// main(argc, argv, envp)
	mov	x1, x20
	mov	x2, x21
	bl	main

	// main() does not return; if it somehow does, leave with its value.
	mov	x8, #93		// exit(status)
	svc	#0
	.size _start, . - _start