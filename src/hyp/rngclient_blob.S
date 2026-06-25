// Embed the rngclient flat image (build/rngclient.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __rng_blob_start
__rng_blob_start:
	.incbin "build/rngclient.bin"
.global __rng_blob_end
__rng_blob_end:
