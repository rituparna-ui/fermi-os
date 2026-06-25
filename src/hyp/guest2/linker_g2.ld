/* VM2 guest linker script. Links at IPA 0x40000000 — the same guest-physical
 * base VM1 (FermiOS) uses — proving stage-2 isolation: the hypervisor maps this
 * IPA to a different HOST PA for VM2. objcopy -O binary produces a flat blob
 * whose byte 0 is IPA 0x40000000. */
ENTRY(_g2_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.g2) *(.text*) }
	.rodata : { *(.rodata.g2) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
