// context_switch(prev: *mut Task, next: *mut Task)   x0=prev, x1=next
// Saves callee-saved GPRs (x19-x30) and SIMD/FP (d8-d15) on prev's stack,
// stores updated sp at prev->sp (offset 0), loads next->sp, restores.
//
// Frame (160 bytes):
//   [sp+0]  x19  [sp+8]  x20 ... [sp+80] x29 [sp+88] x30
//   [sp+96] d8 ... [sp+152] d15
//
// d8-d15 must be preserved because the kernel uses FP/SIMD (core::fmt uses
// SIMD registers); without this a task that formats output would corrupt
// another task's callee-saved FP state.

// Task trampoline: first code a new EL1 task runs after context_switch ret's.
// x19 = real entry point. IRQs are masked on entry (we were switched-in from
// inside the timer IRQ), so unmask first.
.global task_trampoline
task_trampoline:
    msr daifclr, #2     // unmask IRQs
    blr x19             // call the task entry
    bl  task_exit       // clean up + schedule away if it returns
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
    str x2, [x0, #0]    // prev->sp = sp

    ldr x2, [x1, #0]    // sp = next->sp
    mov sp, x2

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
