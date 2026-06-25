// Embed the IPC demo guest's flat image (build/ipc.bin) into the hypervisor,
// same scheme as guest_blob.S / guest2_blob.S. Both the producer and consumer
// VMs run this one image (role selected by x0 at entry).

.section .rodata.guest, "a"
.balign 16
.global __ipc_blob_start
__ipc_blob_start:
	.incbin "build/ipc.bin"
.global __ipc_blob_end
__ipc_blob_end:
