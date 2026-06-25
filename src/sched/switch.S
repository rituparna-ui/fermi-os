// context_switch(prev: *mut Task, next: *mut Task)   x0=prev, x1=next
// Task layout: [0]=kernel sp, [8]=ttbr0 (packed; 0 => kernel task, no swap).
//
// Saves callee-saved GPRs (x19-x30) + FP (d8-d15) on prev's stack, stores sp
// at prev->sp, loads next->sp, swaps TTBR0_EL1 if next is a user task, restores.

// EL0 task trampoline: first code a new user task runs after context_switch
// ret's here. x19 = user entry VA, x20 = user SP (SP_EL0). TTBR0 is already
// the task's user table (swapped by context_switch). Drop to EL0 via eret.
.global user_trampoline
user_trampoline:
    msr daifclr, #2         // unmask IRQs
    msr sp_el0, x20         // user stack pointer
    msr elr_el1, x19        // user entry point
    mov x0, #0              // SPSR = EL0t (all flags/EL bits zero)
    msr spsr_el1, x0
    isb
    eret

// EL1 kernel-task trampoline: x19 = entry fn. No privilege change.
.global task_trampoline
task_trampoline:
    msr daifclr, #2
    blr x19
    bl  task_exit
1:  wfi
    b   1b

.global context_switch
context_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]

    mov x2, sp
    str x2, [x0, #0]        // prev->sp = sp

    ldr x2, [x1, #0]        // sp = next->sp
    mov sp, x2

    // Swap TTBR0_EL1 for user tasks (next->ttbr0 != 0).
    ldr x3, [x1, #8]
    cbz x3, 2f
    msr ttbr0_el1, x3
    dsb ish
    isb
    tlbi vmalle1
    dsb ish
    isb
2:
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8,  d9,  [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160

    ret
