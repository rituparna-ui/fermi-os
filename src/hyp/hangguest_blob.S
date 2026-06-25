// Embed the hangguest flat image (build/hangguest.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __hang_blob_start
__hang_blob_start:
	.incbin "build/hangguest.bin"
.global __hang_blob_end
__hang_blob_end:
