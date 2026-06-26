/* Link the synthetic arm64 Image at the guest's IPA RAM base. QEMU's loader
 * stages the flat binary at IPA 0x40200000 (the Image offset), and the
 * hypervisor enters at that address per the arm64 boot protocol. The header's
 * leading branch jumps over the 64-byte header to the real entry. */
ENTRY(_start)
SECTIONS {
  . = 0x40200000;
  .text : { *(.text._start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}
