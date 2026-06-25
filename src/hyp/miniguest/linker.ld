/* Foreign mini-guest: a standalone EL1 image loaded at guest IPA 0x40000000.
 * Flat (objcopy -O binary) — byte 0 corresponds to 0x40000000. _mini_start
 * (in .text.start) must be first so the hypervisor can enter at the RAM base. */
ENTRY(_mini_start)

SECTIONS
{
  . = 0x40000000;
  .text : {
    *(.text.start)
    *(.text*)
  }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  _mini_end = .;
}
