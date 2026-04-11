// context_switch(task_t *prev, task_t *next)
// x0 = prev, x1 = next
//
// Saves callee-saved registers (x19-x30) + sp onto prev's stack.
// Stores updated sp into prev->sp (offset 0).
// Loads next->sp, restores registers, returns into next task.
//
// Stack frame layout (14 registers × 8 bytes = 112, aligned to 16 → 112):
//   [sp + 0]   x19
//   [sp + 8]   x20
//   [sp + 16]  x21
//   [sp + 24]  x22
//   [sp + 32]  x23
//   [sp + 40]  x24
//   [sp + 48]  x25
//   [sp + 56]  x26
//   [sp + 64]  x27
//   [sp + 72]  x28
//   [sp + 80]  x29 (frame pointer)
//   [sp + 88]  x30 (link register / return address)

// Task trampoline: first code a new task executes after context_switch ret's here.
// x19 = real entry point (set up in sched_create_task's stack frame)
// IRQs are masked (we were switched-in from inside timer IRQ), so unmask first.
.global task_trampoline
task_trampoline:
	msr daifclr, #2		// unmask IRQs
	blr x19			// call the real task entry function
	bl task_exit			// if entry returns, clean up and schedule away
1:	wfi				// safety net (should never reach here)
	b 1b

.global context_switch
context_switch:
	// Save callee-saved registers onto prev's stack
	sub sp, sp, #96
	stp x19, x20, [sp, #0]
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]

	// Save current sp into prev->sp
	mov x2, sp
	str x2, [x0, #0]

	// Load next->sp
	ldr x2, [x1, #0]
	mov sp, x2

	// Restore callee-saved registers from next's stack
	ldp x19, x20, [sp, #0]
	ldp x21, x22, [sp, #16]
	ldp x23, x24, [sp, #32]
	ldp x25, x26, [sp, #48]
	ldp x27, x28, [sp, #64]
	ldp x29, x30, [sp, #80]
	add sp, sp, #96

	ret
