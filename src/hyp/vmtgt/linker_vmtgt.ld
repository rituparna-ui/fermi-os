/* Migration-target guest linker script. Links at IPA 0x40000000 like the other
 * guests; the hypervisor maps this IPA to a per-VM host PA. */
ENTRY(_vmtgt_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.vmtgt) *(.text*) }
	.rodata : { *(.rodata.vmtgt) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
