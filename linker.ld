/* Fermi OS kernel link script — higher-half kernel.
 *
 * The kernel is linked at the upper-half virtual address
 * 0xFFFF_0000_4000_0000 (KERNEL_VA) but loaded by QEMU at physical
 * 0x4000_0000 (KERNEL_PA) via the AT() load-address override. boot.S runs
 * physically (PC == PA) with the MMU off, enables the MMU (TTBR0 identity-maps
 * the low half, TTBR1 maps the high half), then branches to the upper-half
 * kernel_main.
 *
 * Address-of-symbol uses PC-relative adrp, so taking &symbol while PC is
 * physical yields a physical address; the same code post-jump (PC virtual)
 * yields a virtual address. boot.S's literal-pool `ldr =sym` loads the
 * absolute VA, so it subtracts KERNEL_VA_OFFSET explicitly where it needs a PA.
 */
ENTRY(_start)

KERNEL_PA = 0x40000000;
KERNEL_VA = 0xFFFF000040000000;

/* Split LOAD segments by access permission so the kernel ELF doesn't advertise
 * an RWX segment (avoids ld's "LOAD segment with RWX permissions" warning).
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

	/* Upper bound of pages that may be exposed to EL0 user tasks via
	 * mmu_map_user_range. Everything past here (.data, .bss) is
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
	__kernel_end = .;		/* Everything past here is free for the PMM */
}
