// context_switch(prev: *mut Task, next: *mut Task)
//   x0 = prev, x1 = next
//
// Saves callee-saved x19-x30 + d8-d15 onto prev's kernel stack, stores the
// updated SP into prev->sp (offset 0), loads next->sp, swaps TTBR0_EL1 if next
// has user page tables, restores its registers, and returns into the next task.
//
// Context-switch frame (12 GPRs + 8 SIMD = 160 bytes, 16-byte aligned):
//   [sp + 0]   x19      [sp + 48]  x25      [sp + 96]  d8
//   [sp + 8]   x20      [sp + 56]  x26      [sp + 104] d9
//   [sp + 16]  x21      [sp + 64]  x27      [sp + 112] d10
//   [sp + 24]  x22      [sp + 72]  x28      [sp + 120] d11
//   [sp + 32]  x23      [sp + 80]  x29 (fp) [sp + 128] d12
//   [sp + 40]  x24      [sp + 88]  x30 (lr) [sp + 136] d13
//                                           [sp + 144] d14
//                                           [sp + 152] d15
//
// q8-q15's low 64 bits (d8-d15) are callee-saved per AAPCS64 and preserved
// here; the trap frame (vector.S) handles the caller-clobbered SIMD set.

// Task struct field offsets — must match Task in mod.rs.
.equ TASK_SP,    0
.equ TASK_TTBR0, 40

// Task trampoline: first code an EL0 task runs after context_switch returns
// into it. x19 = user entry VA, x20 = user SP (SP_EL0). IRQs are masked (we
// were switched in from inside the timer IRQ), so unmask, then drop to EL0.
.global task_trampoline
task_trampoline:
	msr		daifclr, #2		// unmask IRQs
	msr		sp_el0, x20		// user stack pointer
	msr		elr_el1, x19	// user entry point
	mov		x0, #0			// SPSR = EL0t (all zero)
	msr		spsr_el1, x0
	eret

// Kernel-task trampoline: first code an EL1 kernel-mode task runs. x19 = entry
// fn. No eret / no privilege change — just call it; if it returns, tear down
// via task_exit.
.global kernel_task_trampoline
kernel_task_trampoline:
	msr		daifclr, #2		// unmask IRQs
	blr		x19
	bl		task_exit
1:	b		1b				// unreachable

.global context_switch
context_switch:
	// Save callee-saved registers onto prev's stack.
	sub		sp, sp, #160
	stp		x19, x20, [sp, #0]
	stp		x21, x22, [sp, #16]
	stp		x23, x24, [sp, #32]
	stp		x25, x26, [sp, #48]
	stp		x27, x28, [sp, #64]
	stp		x29, x30, [sp, #80]
	stp		d8,  d9,  [sp, #96]
	stp		d10, d11, [sp, #112]
	stp		d12, d13, [sp, #128]
	stp		d14, d15, [sp, #144]

	// Save current SP into prev->sp.
	mov		x2, sp
	str		x2, [x0, #TASK_SP]

	// Load next->sp.
	ldr		x2, [x1, #TASK_SP]
	mov		sp, x2

	// Swap TTBR0 if next has user page tables. The ttbr0 value already encodes
	// the ASID in [63:48], so the MSR switches the TLB context too. User PTEs
	// are nG-tagged and ASIDs are unique per task, so no TLBI is needed here
	// (sched_reap issues tlbi aside1 when a task dies).
	ldr		x3, [x1, #TASK_TTBR0]
	cbz		x3, 1f			// skip if ttbr0 == 0 (idle / EL1 task)
	msr		ttbr0_el1, x3
	isb
1:

	// Restore callee-saved registers from next's stack.
	ldp		x19, x20, [sp, #0]
	ldp		x21, x22, [sp, #16]
	ldp		x23, x24, [sp, #32]
	ldp		x25, x26, [sp, #48]
	ldp		x27, x28, [sp, #64]
	ldp		x29, x30, [sp, #80]
	ldp		d8,  d9,  [sp, #96]
	ldp		d10, d11, [sp, #112]
	ldp		d12, d13, [sp, #128]
	ldp		d14, d15, [sp, #144]
	add		sp, sp, #160

	ret

// fork_return: first-instruction stub for a forked child. sched_fork copies the
// parent's full 688-byte trap frame to the top of the child's kstack and lays a
// 160-byte context-switch frame just below it; context_switch pops the 160-byte
// frame and ret's here with SP at the trap frame. We mirror the FULL tail of
// exception_common (vector.S) — sysregs, SP_EL0, the caller-saved FP/SIMD set,
// and x0-x30 — then pop the whole 688-byte frame and eret. (Restoring the
// FP/SIMD set matters: the copied frame carries the parent's q0-q7/q16-q31 +
// FPSR/FPCR, and the child must resume with them, not garbage.)
.global fork_return
fork_return:
	ldr		x1, [sp, #248]
	msr		elr_el1, x1
	ldr		x1, [sp, #256]
	msr		spsr_el1, x1

	and		x2, x1, #0xF
	cbnz	x2, 1f
	ldr		x1, [sp, #280]
	msr		sp_el0, x1
1:

	// Restore caller-saved FP/SIMD (mirrors exception_common).
	ldr		x1, [sp, #680]
	msr		fpcr, x1
	ldr		x1, [sp, #672]
	msr		fpsr, x1
	ldp		q30, q31, [sp, #640]
	ldp		q28, q29, [sp, #608]
	ldp		q26, q27, [sp, #576]
	ldp		q24, q25, [sp, #544]
	ldp		q22, q23, [sp, #512]
	ldp		q20, q21, [sp, #480]
	ldp		q18, q19, [sp, #448]
	ldp		q16, q17, [sp, #416]
	ldp		q6,  q7,  [sp, #384]
	ldp		q4,  q5,  [sp, #352]
	ldp		q2,  q3,  [sp, #320]
	ldp		q0,  q1,  [sp, #288]

	ldp		x2,  x3,  [sp, #16]
	ldp		x4,  x5,  [sp, #32]
	ldp		x6,  x7,  [sp, #48]
	ldp		x8,  x9,  [sp, #64]
	ldp		x10, x11, [sp, #80]
	ldp		x12, x13, [sp, #96]
	ldp		x14, x15, [sp, #112]
	ldp		x16, x17, [sp, #128]
	ldp		x18, x19, [sp, #144]
	ldp		x20, x21, [sp, #160]
	ldp		x22, x23, [sp, #176]
	ldp		x24, x25, [sp, #192]
	ldp		x26, x27, [sp, #208]
	ldp		x28, x29, [sp, #224]
	ldr		x30,      [sp, #240]
	ldp		x0,  x1,  [sp, #0]

	add		sp, sp, #688
	eret
