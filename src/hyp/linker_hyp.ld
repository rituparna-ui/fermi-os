/* FermiOS hypervisor (EL2) image linker script.
 *
 * The hyp runs at EL2 with the MMU OFF, so it links and runs at a flat
 * physical address with VA == PA (no higher-half offset, unlike the guest).
 *
 * HYP_PHYS_BASE = 0x250000000 sits inside QEMU's RAM when launched with
 * '-m 9G' (RAM spans 0x40000000..0x268000000) but ABOVE the 8 GiB the guest
 * FermiOS manages (0x40000000..0x240000000). So the hypervisor's text, data
 * and stack never overlap pages the guest PMM hands out — true even at
 * Milestone 1 where stage-2 is off and the guest sees all of physical memory.
 */

ENTRY(_hyp_start)

HYP_PHYS_BASE = 0x250000000;

PHDRS
{
	text   PT_LOAD FLAGS(5);  /* R + X */
	rodata PT_LOAD FLAGS(4);  /* R     */
	data   PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
	. = HYP_PHYS_BASE;

	.text ALIGN(4096) : {
		__hyp_text_start = .;
		*(.text.boot.hyp)
		. = ALIGN(2048);
		*(.text.hyp.vectors)
		*(.text*)
	} :text

	.rodata ALIGN(4096) : {
		*(.rodata.guest)   /* embedded guest flat image (guest_blob.S) */
		*(.rodata*)
	} :rodata

	.data ALIGN(4096) : {
		*(.data*)
	} :data

	.bss ALIGN(4096) (NOLOAD) : {
		__hyp_bss_start = .;
		*(.bss*)
		*(COMMON)
		__hyp_bss_end = .;
	} :data

	/* Per-pCPU EL2 stacks: 4 x (64 KiB stack + 4 KiB guard page). pCPU n's stack
	 * TOP = __hyp_stacks_base + n*0x11000 + 0x10000. The guard page is slack so a
	 * small overflow corrupts the guard rather than a neighbour's stack. pCPU0's
	 * stack top keeps the same offset as the old single stack. */
	. = ALIGN(4096);
	__hyp_stacks_base = .;
	. = . + 0x11000 * 4;		/* HYP_MAX_PCPUS=4 x (64 KiB + 4 KiB guard) */
	__hyp_stacks_end = .;
	__hyp_stack_top = __hyp_stacks_base + 0x10000;	/* pCPU0 stack top */

	. = ALIGN(4096);
	__hyp_end = .;
}
