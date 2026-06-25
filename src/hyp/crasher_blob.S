// Embed the crasher guest flat image (build/crasher.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __crasher_blob_start
__crasher_blob_start:
	.incbin "build/crasher.bin"
.global __crasher_blob_end
__crasher_blob_end:
