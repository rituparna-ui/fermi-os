// context_switch(task_t *prev, task_t *next)
// x0 = prev, x1 = next
//
// Saves callee-saved registers (x19-x30) + sp onto prev's stack.
// Stores updated sp into prev->sp (offset 0).
// Loads next->sp, restores registers, returns into next task.
// Also swaps TTBR0_EL1 for per-task user page tables.
//
// Stack frame layout (12 GPRs + 8 SIMD = 160 bytes):
//   [sp + 0]    x19
//   [sp + 8]    x20
//   [sp + 16]   x21
//   [sp + 24]   x22
//   [sp + 32]   x23
//   [sp + 40]   x24
//   [sp + 48]   x25
//   [sp + 56]   x26
//   [sp + 64]   x27
//   [sp + 72]   x28
//   [sp + 80]   x29 (frame pointer)
//   [sp + 88]   x30 (link register / return address)
//   [sp + 96]   d8   (callee-saved SIMD/FP, AAPCS64)
//   [sp + 104]  d9
//   [sp + 112]  d10
//   [sp + 120]  d11
//   [sp + 128]  d12
//   [sp + 136]  d13
//   [sp + 144]  d14
//   [sp + 152]  d15

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

// Kernel-task trampoline: first code a kernel-mode (EL1) task runs.
// x19 = entry function pointer. We just call it directly — no eret, no
// privilege change. If the entry returns, fall through to task_exit so
// the task is cleanly torn down by the scheduler.
.global kernel_task_trampoline
kernel_task_trampoline:
	msr daifclr, #2		// unmask IRQs
	blr x19
	bl  task_exit
1:	b 1b			// unreachable


.global context_switch
context_switch:
	// Save callee-saved registers onto prev's stack
	sub sp, sp, #160
	stp x19, x20, [sp, #0]
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]
	// Save callee-saved SIMD/FP regs (lower 64 bits of v8–v15)
	stp d8,  d9,  [sp, #96]
	stp d10, d11, [sp, #112]
	stp d12, d13, [sp, #128]
	stp d14, d15, [sp, #144]

	// Save current sp into prev->sp
	mov x2, sp
	str x2, [x0, #TASK_SP]

	// Load next->sp
	ldr x2, [x1, #TASK_SP]
	mov sp, x2

	// --- Switch TTBR0 if next task has user page tables ---
	// Each task's ttbr0 already encodes its ASID in [63:48], so the MSR
	// also switches the TLB context. With user mappings tagged nG=1 in
	// mmu_map_user_range and ASIDs assigned uniquely per task, no TLBI
	// is required: stale entries from the previous user task remain in
	// the TLB harmlessly under their own ASID and the new task starts
	// with zero entries cached for its ASID. (sched_reap issues a
	// `tlbi aside1` when a task dies so its ASID's entries are dropped
	// before the page tables are pmm_free'd.)
	ldr x3, [x1, #TASK_TTBR0]
	cbz x3, 1f			// skip if ttbr0 == 0 (idle / EL1 task)

	msr ttbr0_el1, x3
	isb
1:

	// Restore callee-saved registers from next's stack
	ldp x19, x20, [sp, #0]
	ldp x21, x22, [sp, #16]
	ldp x23, x24, [sp, #32]
	ldp x25, x26, [sp, #48]
	ldp x27, x28, [sp, #64]
	ldp x29, x30, [sp, #80]
	// Restore callee-saved SIMD/FP regs
	ldp d8,  d9,  [sp, #96]
	ldp d10, d11, [sp, #112]
	ldp d12, d13, [sp, #128]
	ldp d14, d15, [sp, #144]
	add sp, sp, #160

	ret


// fork_return: first-instruction stub for a forked child task. The caller
// (sched_fork) lays out a 288-byte trap frame at the top of the child's
// kstack and a 160-byte context_switch frame just below it; context_switch
// pops the 160-byte frame, ret's into here with sp pointing at the trap
// frame. We then mirror the tail of exception_common in vector.S to
// restore ELR/SPSR/SP_EL0/x0–x30 and eret to user space.
.global fork_return
fork_return:
	// restore system registers
	ldr x1, [sp, #248]
	msr elr_el1, x1
	ldr x1, [sp, #256]
	msr spsr_el1, x1

	// Restore SP_EL0 if returning to EL0 (SPSR.M[3:0] == 0)
	and x2, x1, #0xF
	cbnz x2, 1f
	ldr x1, [sp, #280]
	msr sp_el0, x1
1:

	// restore general-purpose registers
	ldp x2,  x3,  [sp, #16]
	ldp x4,  x5,  [sp, #32]
	ldp x6,  x7,  [sp, #48]
	ldp x8,  x9,  [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x19, [sp, #144]
	ldp x20, x21, [sp, #160]
	ldp x22, x23, [sp, #176]
	ldp x24, x25, [sp, #192]
	ldp x26, x27, [sp, #208]
	ldp x28, x29, [sp, #224]
	ldr x30,      [sp, #240]
	ldp x0, x1,   [sp, #0]

	add sp, sp, #288

	eret
