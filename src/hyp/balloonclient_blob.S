// Embed the balloonclient guest flat image (build/balloonclient.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __balloon_blob_start
__balloon_blob_start:
	.incbin "build/balloonclient.bin"
.global __balloon_blob_end
__balloon_blob_end:
