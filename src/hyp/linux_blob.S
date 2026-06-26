/* Embed a real aarch64 Linux kernel Image (linux-image/Image) into the
 * hypervisor so it can be booted as an EL1 guest (M28). Image is the raw
 * arch/arm64/boot/Image with its 64-byte AArch64 boot header.
 *
 * Only compiled when linux-image/Image exists (Makefile adds this file +
 * defines HAVE_LINUX_IMAGE); otherwise the hypervisor builds without Linux. */
/* NB: do not name the section ".rodata.linux" — cpp predefines `linux`=1, so
 * the bare token would expand. Use a token cpp won't touch. */
.section .rodata.lxkern
.balign 0x10000
.global __linux_blob_start
__linux_blob_start:
	.incbin "linux-image/Image"
.global __linux_blob_end
__linux_blob_end:

.section .rodata
.balign 8
.global __linux_blob_size
__linux_blob_size:
	.quad __linux_blob_end - __linux_blob_start
