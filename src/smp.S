// Secondary-core entry. PSCI CPU_ON starts the core at EL1 with the MMU OFF,
// so this runs at the physical address. Symbols are linked at the upper-half
// VA; convert to physical by subtracting KERNEL_VA_OFFSET. Set a stack, call
// the Rust secondary handler, then park.
.section .text
.globl secondary_start
.equ KERNEL_VA_OFFSET, 0xFFFF000000000000
secondary_start:
    msr     daifset, #0xf
    mov     x0, #(3 << 20)          // enable FP/SIMD (CPACR_EL1.FPEN)
    msr     cpacr_el1, x0
    isb
    ldr     x0, =secondary_stack_top
    ldr     x1, =KERNEL_VA_OFFSET
    sub     x0, x0, x1              // physical stack top
    mov     sp, x0
    bl      rust_secondary
1:  wfi
    b       1b
