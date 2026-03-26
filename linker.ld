ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFF000000000000;
KERNEL_PHYS_BASE = 0x40000000;

SECTIONS
{
	/*
	 * Boot code and boot page tables live at physical addresses.
	 * They run before the MMU is enabled and must be identity-mapped.
	 */
	. = KERNEL_PHYS_BASE;

	.text.boot ALIGN(4096) : {
		__boot_start = .;
		*(.text.boot)
		__boot_end = .;
	}

	/* Boot-time BSS (page tables) — still at physical addresses */
	.bss.boot ALIGN(4096) (NOLOAD) : {
		*(.bss.boot)
	}

	/*
	 * Record where the physical sections end so we know the
	 * boundary before the higher-half VMA jump.
	 */
	. = ALIGN(4096);
	__phys_end = .;

	/*
	 * Everything from here on is the higher-half kernel.
	 * VMA = KERNEL_VIRT_BASE + phys offset
	 * LMA = physical address (so the bootloader/QEMU loads it correctly)
	 */
	. += KERNEL_VIRT_BASE;

	.text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
		__text_start = .;
		*(.text*)
		__text_end = .;
	}

	.rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
		__rodata_start = .;
		*(.rodata*)
		__rodata_end = .;
	}

	.data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
		__data_start = .;
		*(.data*)
		__data_end = .;
	}

	.bss ALIGN(4096) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
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

	/*
	 * Physical aliases — used by boot.S before the MMU is on.
	 */
	__bss_start_phys = __bss_start - KERNEL_VIRT_BASE;
	__bss_end_phys   = __bss_end   - KERNEL_VIRT_BASE;
	__kernel_end_phys = __kernel_end - KERNEL_VIRT_BASE;
}
