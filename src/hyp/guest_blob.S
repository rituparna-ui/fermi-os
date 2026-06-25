// Embed the reduced-RAM FermiOS guest image (build/guest.bin) inside the
// hypervisor image so QEMU '-kernel build/kernel.elf' is the only ROM loaded.
//
// build/guest.bin is `objcopy -O binary build/guest.elf`: a flat image laid out
// by LMA starting at guest physical 0x40000000, with inter-segment gaps zero-
// padded — so a single memcpy of the whole blob to the guest's RAM base
// reconstructs every PT_LOAD at its correct offset. The guest's .bss tail
// (memsz>filesz) is not in the blob; the guest zeroes its own .bss in
// early_init/zero_bss.
//
// This file is the ONLY source excluded from the guest build (it would
// otherwise embed the guest into itself recursively).

.section .rodata.guest, "a"
.balign 4096
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
