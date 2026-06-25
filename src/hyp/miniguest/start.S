// Foreign mini-guest entry (milestone 22). This is a STANDALONE AArch64 EL1
// program — it shares no code with FermiOS. It follows the standard AArch64
// boot protocol: the hypervisor enters it with x0 = physical/IPA address of a
// DTB. We stash the DTB pointer, set up a stack at the top of our image's RAM
// window, and call mini_main(dtb).
//
// Linked at 0x40000000 (the guest RAM base the hypervisor maps), MMU off.

.section .text.start, "ax"
.global _mini_start
_mini_start:
	mov	x19, x0			// preserve DTB pointer across stack setup

	// Stack: top of a small region just below 1 MiB into our RAM window.
	movz	x1, #0x0000
	movk	x1, #0x4000, lsl #16	// 0x40000000 (RAM base)
	movz	x2, #0x0000
	movk	x2, #0x0010, lsl #16	// + 0x100000 (1 MiB)
	add	x1, x1, x2
	mov	sp, x1

	mov	x0, x19			// x0 = DTB pointer
	bl	mini_main
1:	wfi
	b	1b
