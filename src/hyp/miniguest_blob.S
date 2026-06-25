// Embed the standalone foreign mini-guest image (build/miniguest.bin) into the
// hypervisor. It is a separate AArch64 EL1 program (src/hyp/miniguest/, own
// linker.ld + _mini_start) flattened with objcopy -O binary so byte 0 is guest
// IPA 0x40000000. The hypervisor copies the whole blob to the guest RAM base
// and enters at 0x40000000 with x0 = DTB pointer (M22).
.section .rodata.miniguest, "a"
.balign 16
.global __miniguest_blob_start
__miniguest_blob_start:
	.incbin "build/miniguest.bin"
.global __miniguest_blob_end
__miniguest_blob_end:
