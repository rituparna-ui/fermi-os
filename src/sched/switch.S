// context_switch(prev: *mut Task, next: *mut Task)
//   x0 = prev, x1 = next
//
// Saves callee-saved registers (x19-x30) onto prev's kernel stack, stores the
// updated SP into prev->sp (offset 0), loads next->sp, restores its registers,
// and returns into the next task.
//
// Context-switch frame (12 GPRs * 8 = 96 bytes, 16-byte aligned):
//   [sp + 0]   x19      [sp + 48]  x25
//   [sp + 8]   x20      [sp + 56]  x26
//   [sp + 16]  x21      [sp + 64]  x27
//   [sp + 24]  x22      [sp + 72]  x28
//   [sp + 32]  x23      [sp + 80]  x29 (frame pointer)
//   [sp + 40]  x24      [sp + 88]  x30 (link register / return address)
//
// This is the kernel-task scheduler frame (no SIMD yet, no TTBR0 swap); the
// EL0/user variant with d8-d15 and per-task TTBR0 is introduced later, with the
// userspace work.

// task struct field offset — must match Task in mod.rs.
.equ TASK_SP, 0

// Task trampoline: the first code a freshly-created task runs after
// context_switch returns into it. x19 = entry fn. IRQs are masked (we were
// switched in from inside the timer IRQ), so unmask, call the entry, and on
// return fall through to task_exit for clean teardown.
.global task_trampoline
task_trampoline:
	msr		daifclr, #2		// unmask IRQs
	blr		x19				// call the real task entry
	bl		task_exit		// if it returns, tear the task down
1:	wfi						// safety net — should never reach here
	b		1b

.global context_switch
context_switch:
	// Save callee-saved registers onto prev's stack.
	sub		sp, sp, #96
	stp		x19, x20, [sp, #0]
	stp		x21, x22, [sp, #16]
	stp		x23, x24, [sp, #32]
	stp		x25, x26, [sp, #48]
	stp		x27, x28, [sp, #64]
	stp		x29, x30, [sp, #80]

	// Save current SP into prev->sp.
	mov		x2, sp
	str		x2, [x0, #TASK_SP]

	// Load next->sp.
	ldr		x2, [x1, #TASK_SP]
	mov		sp, x2

	// Restore callee-saved registers from next's stack.
	ldp		x19, x20, [sp, #0]
	ldp		x21, x22, [sp, #16]
	ldp		x23, x24, [sp, #32]
	ldp		x25, x26, [sp, #48]
	ldp		x27, x28, [sp, #64]
	ldp		x29, x30, [sp, #80]
	add		sp, sp, #96

	ret
