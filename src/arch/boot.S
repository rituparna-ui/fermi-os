// Kernel entry point.
//
// QEMU enters here at EL1 (gic-version=3, no virtualization) with the MMU off.
// We set up the stack pointer and hand control to the Rust kernel. The C
// original branched straight into kernel_main; the Rust port routes through a
// small assembly trampoline so we keep a single, well-defined entry.

.section .text.boot
.global _start

_start:
	// Park all secondary CPUs: only the primary (MPIDR Aff0 == 0) proceeds.
	mrs		x1, mpidr_el1
	and		x1, x1, #0xff
	cbnz	x1, halt

	// Set up the kernel stack (grows down from __stack_top).
	ldr		x0, =__stack_top
	mov		sp, x0

	// Zero the .bss section. QEMU's -kernel loader does not clear it, and the
	// kernel's statics (locks, allocators) rely on zero initialization.
	ldr		x0, =__bss_start
	ldr		x1, =__bss_end
zero_bss:
	cmp		x0, x1
	b.hs	bss_done
	str		xzr, [x0], #8
	b		zero_bss
bss_done:

	// Into Rust. kmain never returns.
	bl		kmain

halt:
	wfe
	b		halt
