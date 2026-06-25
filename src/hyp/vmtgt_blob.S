// Embed the migration-target guest flat image (build/vmtgt.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __vmtgt_blob_start
__vmtgt_blob_start:
	.incbin "build/vmtgt.bin"
.global __vmtgt_blob_end
__vmtgt_blob_end:
