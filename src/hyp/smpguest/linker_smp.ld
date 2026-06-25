/* smpguest linker script. Links at IPA 0x40000000 like every guest; the boot
 * section (containing _smp_start) is placed FIRST so the hyp's eret to
 * 0x40000000 lands on the entry point. */
ENTRY(_smp_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.smp) *(.text*) }
	.rodata : { *(.rodata.smp) *(.rodata*) }
	.data : { *(.data.smp) *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
