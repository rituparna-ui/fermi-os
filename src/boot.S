// Fermi OS boot stub (aarch64 assembly).
// Mirrors the original src/boot.S: set up the stack and jump into the kernel.
// Additions over the very first C commit: mask interrupts and zero the BSS
// before entering Rust (Rust statics in .bss require a zeroed region).

.section .text.boot
.globl _start
_start:
    // Mask all interrupts (D, A, I, F) until we are ready.
    msr     daifset, #0xf

    // Set up the boot stack.
    ldr     x0, =__stack_top
    mov     sp, x0

    // Zero the BSS: [__bss_start, __bss_end).
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:
    cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    // Enter the Rust kernel.
    bl      rust_main

    // rust_main should never return; park the core if it does.
3:
    wfe
    b       3b
