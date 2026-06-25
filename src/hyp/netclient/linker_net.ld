/* netclient linker script. Links at IPA 0x40000000 like every guest. */
ENTRY(_net_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.net) *(.text*) }
	.rodata : { *(.rodata.net) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
