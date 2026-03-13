.section ".text.boot"
.global _start

_start:
  LDR x0, =__stack_top
  MOV sp, x0

  B kernel_main
  # BL kernel_main
  # B panic_handler
