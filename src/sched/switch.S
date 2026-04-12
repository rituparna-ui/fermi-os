// context_switch(task_t *prev, task_t *next)
// x0 = prev, x1 = next
//
// Saves callee-saved registers (x19-x30) + sp onto prev's stack.
// Stores updated sp into prev->sp (offset 0).
// Loads next->sp, restores registers, returns into next task.
// Also swaps TTBR0_EL1 for per-task user page tables.
//
// Stack frame layout (12 registers × 8 bytes = 96):
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

// task_t field offsets — must match sched.h
.equ TASK_SP,       0
.equ TASK_TTBR0,    40   // offsetof(task_t, ttbr0)

// Task trampoline: first code a new task executes after context_switch ret's here.
// x19 = user entry point (VA in user address space)
// x20 = user stack pointer (SP_EL0)
// IRQs are masked (we were switched-in from inside timer IRQ), so unmask first.
// Then drop to EL0 via eret.
.global task_trampoline
task_trampoline:
	msr daifclr, #2		// unmask IRQs

	// Set up for eret to EL0
	msr sp_el0, x20			// user stack pointer
	msr elr_el1, x19		// user entry point
	mov x0, #0			// SPSR = EL0t (all bits zero)
	msr spsr_el1, x0

	eret

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
	str x2, [x0, #TASK_SP]

	// Load next->sp
	ldr x2, [x1, #TASK_SP]
	mov sp, x2

	// --- Switch TTBR0 if next task has user page tables ---
	ldr x3, [x1, #TASK_TTBR0]
	cbz x3, 1f			// skip if ttbr0 == 0 (idle task)

	msr ttbr0_el1, x3
	isb
	tlbi vmalle1
	dsb ish
	isb
1:

	// Restore callee-saved registers from next's stack
	ldp x19, x20, [sp, #0]
	ldp x21, x22, [sp, #16]
	ldp x23, x24, [sp, #32]
	ldp x25, x26, [sp, #48]
	ldp x27, x28, [sp, #64]
	ldp x29, x30, [sp, #80]
	add sp, sp, #96

	ret
