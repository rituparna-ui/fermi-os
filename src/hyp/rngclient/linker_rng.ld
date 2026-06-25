/* rngclient linker script. Links at IPA 0x40000000 like every guest; the
 * virtqueue structures live at fixed higher IPAs in its RAM window. */
ENTRY(_rng_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.rng) *(.text*) }
	.rodata : { *(.rodata.rng) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
