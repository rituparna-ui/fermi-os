ENTRY(_start)

KERNEL_PA = 0x40000000;
KERNEL_VA = 0xFFFF000040000000;

/* Split LOAD segments by access permissions so the kernel ELF doesn't
 * advertise an RWX segment (which is what triggers ld's
 * "LOAD segment with RWX permissions" warning).
 *
 *   text   PT_LOAD R-X  : .text.boot, .text.vectors, .text*
 *   rodata PT_LOAD R--  : .rodata*
 *   data   PT_LOAD RW-  : .data*, .bss*
 */
PHDRS
{
	text   PT_LOAD FLAGS(5);  /* R + X */
	rodata PT_LOAD FLAGS(4);  /* R     */
	data   PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
	. = KERNEL_VA;

	.text ALIGN(4096) : AT(KERNEL_PA) {
		__text_start = .;
		*(.text.boot)
		. = ALIGN(2048);
		*(.text.vectors)
		*(.text*)
	} :text

	.rodata ALIGN(4096) : {
		*(.rodata*)
	} :rodata

	/* Marks the upper bound of pages that may be exposed to EL0 user tasks
	 * via mmu_map_user_range. Everything past here (.data, .bss) is
	 * kernel-private and must NEVER be mapped into a user TTBR0. */
	. = ALIGN(4096);
	__user_text_end = .;

	.data ALIGN(4096) : {
		*(.data*)
	} :data

	.bss ALIGN(4096) (NOLOAD) : {
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		__bss_end = .;
	} :data

	. = ALIGN(4096);
	. = . + 0x10000;		/* 64 KiB kernel stack */
	__stack_top = .;		/* Stack grows downward */

	. = ALIGN(4096);
	__kernel_end = .;		/* Everything after this is free for PMM */
}
