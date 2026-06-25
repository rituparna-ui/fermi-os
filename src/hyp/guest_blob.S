// Embed the guest FermiOS flat image (build/guest.bin) inside the hypervisor
// image. The Makefile objcopy's build/kernel.elf to a flat binary whose byte 0
// corresponds to guest physical address 0x40000000, with inter-segment gaps
// preserved as zero padding — so a single memcpy of the whole blob to
// 0x40000000 reconstructs every guest PT_LOAD at its correct physical address.
// (The guest's .bss tail, which is memsz>filesz, is not in the blob; the guest
// zeroes its own .bss in early_init/zero_bss.)
//
// Embedding the guest this way means QEMU '-kernel build/hyp.elf' is the ONLY
// image QEMU loads, so the auto-placed DTB at 0x40000000 no longer collides
// with a separately-loaded guest ELF.

.section .rodata.guest, "a"
.balign 16
.global __guest_blob_start
__guest_blob_start:
	.incbin "build/guest.bin"
.global __guest_blob_end
__guest_blob_end:

.section .rodata
.balign 8
.global __guest_blob_size
__guest_blob_size:
	.quad __guest_blob_end - __guest_blob_start
