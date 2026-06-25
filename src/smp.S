// Secondary-core entry (PSCI CPU_ON starts here at EL1, MMU off, physical PC).
.section .text
.globl secondary_start
.equ KERNEL_VA_OFFSET, 0xFFFF000000000000
secondary_start:
    msr     daifset, #0xf
    mov     x0, #(3 << 20)              // FP/SIMD enable
    msr     cpacr_el1, x0
    isb
    ldr     x0, =secondary_stack_top    // physical stack
    ldr     x1, =KERNEL_VA_OFFSET
    sub     x0, x0, x1
    mov     sp, x0
    bl      rust_secondary_early        // enable MMU (returns, low-identity)
    ldr     x0, =secondary_stack_top    // higher-half stack
    mov     sp, x0
    ldr     x1, =rust_secondary_high    // higher-half entry
    br      x1
1:  wfi
    b       1b
