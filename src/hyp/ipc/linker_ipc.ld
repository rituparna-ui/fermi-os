/* IPC demo guest linker script. Links at IPA 0x40000000 like the other guests;
 * the hypervisor maps this IPA to a per-VM host PA. The shared page lives at a
 * separate IPA (0x50000000) mapped by the hyp to a common host PA. */
ENTRY(_ipc_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.ipc) *(.text*) }
	.rodata : { *(.rodata.ipc) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
