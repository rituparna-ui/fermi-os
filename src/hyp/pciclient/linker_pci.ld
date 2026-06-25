ENTRY(_pci_start)
SECTIONS {
	. = 0x40000000;
	.text : { *(.text.boot.pci) *(.text*) }
	.rodata : { *(.rodata.pci) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
