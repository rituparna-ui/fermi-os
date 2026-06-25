// Embed the dom0 control-guest flat image (build/dom0.bin) into the hypervisor.

.section .rodata.guest, "a"
.balign 16
.global __dom0_blob_start
__dom0_blob_start:
	.incbin "build/dom0.bin"
.global __dom0_blob_end
__dom0_blob_end:
