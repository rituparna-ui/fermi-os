/* balloonclient linker script. Links at IPA 0x40000000 like every guest; the
 * boot section (with _balloon_start) is placed FIRST so the hyp's eret to
 * 0x40000000 lands on the entry point. */
ENTRY(_balloon_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.balloon) *(.text*) }
	.rodata : { *(.rodata.balloon) *(.rodata*) }
	.data : { *(.data.balloon) *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
