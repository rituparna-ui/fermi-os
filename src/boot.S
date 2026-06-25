// Fermi OS boot stub (aarch64) — higher-half.
//
// Symbols are linked at upper-half VAs. Before the MMU is on, PC is physical,
// so we convert absolute symbol addresses to physical by subtracting
// KERNEL_VA_OFFSET. early_init enables the MMU (identity-low via TTBR0,
// upper-half via TTBR1); afterwards we switch SP/PC to the upper half and
// branch into kernel_main, which runs entirely from TTBR1.

.section .text.boot
.globl _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000

_start:
    // Mask all interrupts until we are ready.
    msr     daifset, #0xf

    // Enable FP/SIMD (CPACR_EL1.FPEN = 0b11) — LLVM uses SIMD regs (core::fmt,
    // memcpy), which would otherwise trap with ESR EC=0x7.
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Physical boot stack: __stack_top is an upper-half VA; subtract offset.
    ldr     x0, =__stack_top
    ldr     x1, =KERNEL_VA_OFFSET
    sub     x0, x0, x1
    mov     sp, x0

    // Zero BSS using PHYSICAL addresses (statics aren't reachable until MMU).
    ldr     x1, =__bss_start
    ldr     x3, =KERNEL_VA_OFFSET
    sub     x1, x1, x3
    ldr     x2, =__bss_end
    sub     x2, x2, x3
1:
    cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    // early_init runs at physical PC (bl is PC-relative). It sets up UART/PMM
    // and enables the MMU. Rust static accesses use PC-relative adrp/add, so
    // they resolve to physical addresses while PC is physical.
    bl      early_init

    // MMU is on; TTBR1 maps the upper half. Switch SP and PC to upper-half VAs.
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x1, =kernel_main
    br      x1

3:
    wfe
    b       3b
