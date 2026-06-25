// Kernel entry point — higher-half boot.
//
// The kernel is linked at upper-half VAs (0xFFFF_0000_4000_0000) but loaded and
// entered physically at 0x4000_0000 with the MMU off. So `ldr =sym` literal
// loads yield VAs that we must convert to PAs (subtract KERNEL_VA_OFFSET) until
// the MMU is enabled. `early_init` builds the page tables and turns the MMU on;
// after it returns we reload SP with the upper-half VA and branch to the
// upper-half kmain.

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000

.section .text.boot
.global _start

_start:
	// Park all secondary CPUs: only the primary (MPIDR Aff0 == 0) proceeds.
	mrs		x1, mpidr_el1
	and		x1, x1, #0xff
	cbnz	x1, halt

	// Physical stack top: symbol is a VA, subtract the offset (MMU is off).
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
