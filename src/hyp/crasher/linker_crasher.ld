/* crasher guest linker script. Links at IPA 0x40000000 like every guest. */
ENTRY(_crasher_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.crasher) *(.text*) }
	.rodata : { *(.rodata.crasher) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
