/* Fermi OS kernel link script.
 *
 * QEMU's `-kernel` loads the image at physical 0x4000_0000 and enters at the
 * ELF entry point. At this early stage the kernel runs identity-mapped (MMU
 * off), linked at its physical load address. The higher-half split (TTBR1 at
 * 0xFFFF_0000_4000_0000) is introduced later, mirroring the original project's
 * commit progression.
 */
ENTRY(_start)

KERNEL_PA = 0x40000000;

SECTIONS
{
	. = KERNEL_PA;

	.text ALIGN(4096) : {
		__text_start = .;
		*(.text.boot)
		*(.text*)
	}

	.rodata ALIGN(4096) : {
		*(.rodata*)
	}

	.data ALIGN(4096) : {
		*(.data*)
	}

	.bss ALIGN(4096) (NOLOAD) : {
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		__bss_end = .;
	}

	. = ALIGN(4096);
	. = . + 0x10000;		/* 64 KiB kernel stack */
	__stack_top = .;		/* Stack grows downward */

	. = ALIGN(4096);
	__kernel_end = .;		/* Everything past here is free for the PMM */
}
