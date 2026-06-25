/* dom0 control guest linker script. Links at IPA 0x40000000 like the other
 * guests; the hypervisor maps this IPA to a per-VM host PA. */
ENTRY(_dom0_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.dom0) *(.text*) }
	.rodata : { *(.rodata.dom0) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
