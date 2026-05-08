ENTRY(_start)

KERNEL_PA = 0x40000000;
KERNEL_VA = 0xFFFF000040000000;

SECTIONS
{
	. = KERNEL_VA;

	.text ALIGN(4096) : AT(KERNEL_PA) {
		*(.text.boot)
		. = ALIGN(2048);
		*(.text.vectors)
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
	__kernel_end = .;		/* Everything after this is free for PMM */
}