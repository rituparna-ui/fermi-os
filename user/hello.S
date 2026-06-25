// Standalone aarch64 EL0 program, linked as ET_EXEC at 0x400000.
// Uses only the Fermi syscall ABI (svc #0, x8=num). Loaded by the kernel's
// ELF64 loader into a fresh user address space.
.section .text
.global _start
_start:
    // write(1, msg, len)
    mov x8, #1
    mov x0, #1
    adr x1, msg
    mov x2, #(msg_end - msg)
    svc #0
    // getpid()
    mov x8, #7
    svc #0
    // exit(0)
    mov x8, #4
    mov x0, #0
    svc #0
1:  b 1b
msg:
    .ascii "    [elf] hello from a loaded ELF64 binary running at EL0!\n"
msg_end:
