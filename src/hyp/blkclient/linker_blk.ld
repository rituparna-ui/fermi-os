/* blkclient linker script. Links at IPA 0x40000000 like every guest. */
ENTRY(_blk_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.blk) *(.text*) }
	.rodata : { *(.rodata.blk) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
