// Embed the smpguest 2-vCPU SMP guest flat image (build/smpguest.bin) into the hyp.

.section .rodata.guest, "a"
.balign 16
.global __smp_blob_start
__smp_blob_start:
	.incbin "build/smpguest.bin"
.global __smp_blob_end
__smp_blob_end:
