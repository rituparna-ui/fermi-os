// Fermi OS boot stub (aarch64 assembly).
// Mirrors the original src/boot.S: set up the stack and jump into the kernel.
// Additions over the very first C commit: mask interrupts and zero the BSS
// before entering Rust (Rust statics in .bss require a zeroed region).

.section .text.boot
.globl _start
_start:
    // Mask all interrupts (D, A, I, F) until we are ready.
    msr     daifset, #0xf

    // Enable FP/SIMD access at EL1/EL0 (CPACR_EL1.FPEN = 0b11). Without this,
    // any Advanced SIMD/FP instruction traps with ESR EC=0x7. The Rust/LLVM
    // codegen uses SIMD registers (e.g. for memcpy and 16-byte fat-pointer
    // moves in core::fmt), so this must be on before any Rust code runs.
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

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
