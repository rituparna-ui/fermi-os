// Embed the netclient flat image (build/netclient.bin) into the hyp.
.section .rodata.guest, "a"
.balign 16
.global __net_blob_start
__net_blob_start:
	.incbin "build/netclient.bin"
.global __net_blob_end
__net_blob_end:
