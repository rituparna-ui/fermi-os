.section .rodata.guest, "a"
.balign 16
.global __pci_blob_start
__pci_blob_start:
	.incbin "build/pciclient.bin"
.global __pci_blob_end
__pci_blob_end:
