// Embed VM2's flat image (build/guest2.bin) into the hypervisor, same scheme
// as guest_blob.S for VM1. The hyp memcpy's it to VM2's host RAM base before
// starting the scheduler.

.section .rodata.guest, "a"
.balign 16
.global __guest2_blob_start
__guest2_blob_start:
	.incbin "build/guest2.bin"
.global __guest2_blob_end
__guest2_blob_end:
