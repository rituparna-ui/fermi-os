/* hangguest linker script. Links at IPA 0x40000000 like every guest. */
ENTRY(_hang_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.hang) *(.text*) }
	.rodata : { *(.rodata.hang) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
