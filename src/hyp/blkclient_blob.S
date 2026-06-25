// Embed the blkclient flat image (build/blkclient.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __blk_blob_start
__blk_blob_start:
	.incbin "build/blkclient.bin"
.global __blk_blob_end
__blk_blob_end:
