//! Preemptive round-robin scheduler (kernel-mode EL1 tasks).
//!
//! This is the first scheduler increment, mirroring the original C commit
//! a7dc401: a circular run queue of kernel tasks, timer-driven preemption,
//! per-task kernel stacks, context switching via callee-saved register
//! save/restore, and a dead-task reaper. EL0/user tasks, per-task TTBR0/ASID,
//! fork, and fd tables layer on later alongside the syscall subsystem.
//!
//! Concurrency (§2.5 of the port plan): the run queue is mutated only with IRQs
//! masked and on a single core, so it is held in `static mut` raw pointers
//! rather than behind a lock — taking a lock in the timer-IRQ schedule path
//! would risk deadlock. Every mutation site masks IRQs or is already in IRQ
//! context. `// SAFETY (single-core)` marks each such access.

#![allow(dead_code)]

use crate::mm::consts::{phys_to_virt, PAGE_SIZE};
use crate::mm::{heap, pmm};
use crate::{exception, kprintln};
use core::arch::global_asm;

global_asm!(include_str!("switch.S"));

extern "C" {
    fn context_switch(prev: *mut Task, next: *mut Task);
    fn task_trampoline();
}

/// 16 KiB per-task kernel stack.
pub const TASK_STACK_PAGES: u64 = 4;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TaskState {
    Ready,
    Running,
    Sleeping,
    Dead,
}

/// A schedulable task. `#[repr(C)]` with `sp` at offset 0 — switch.S reads/
/// writes `prev->sp` / `next->sp` at `TASK_SP = 0`.
#[repr(C)]
pub struct Task {
    pub sp: u64, // offset 0: kernel SP (context_switch saves/restores here)
    pub pid: u64,
    pub state: TaskState,
    pub sleep_until: u64,
    pub stack_phys: u64,
    pub name: [u8; 16],
    pub next: *mut Task,
}

// Freeze the asm-critical offset.
const _: () = assert!(core::mem::offset_of!(Task, sp) == 0);

impl Task {
    const fn zeroed() -> Self {
        Self {
            sp: 0,
            pid: 0,
            state: TaskState::Ready,
            sleep_until: 0,
            stack_phys: 0,
            name: [0; 16],
            next: core::ptr::null_mut(),
        }
    }
}

/// Type of a kernel task entry function.
pub type TaskEntry = extern "C" fn();

// Scheduler state. Single-core + IRQ-masked mutation, hence raw statics.
static mut IDLE_TASK: Task = Task::zeroed();
static mut CURRENT: *mut Task = core::ptr::null_mut();
static mut NEXT_PID: u64 = 0;
static mut DEAD_LIST: *mut Task = core::ptr::null_mut();

fn copy_name(dst: &mut [u8; 16], src: &str) {
    let mut i = 0;
    for b in src.bytes() {
        if i >= 15 {
            break;
        }
        dst[i] = b;
        i += 1;
    }
}

/// Mask IRQs (DAIF.I), returning the prior DAIF for restore.
#[inline]
fn irq_save() -> u64 {
    let daif: u64;
    unsafe {
        core::arch::asm!("mrs {}, daif", out(reg) daif, options(nomem, nostack));
        core::arch::asm!("msr daifset, #2", options(nomem, nostack));
    }
    daif
}

#[inline]
fn irq_restore(daif: u64) {
    unsafe {
        core::arch::asm!("msr daif, {}", in(reg) daif, options(nomem, nostack));
    }
}

/// Initialize the scheduler: set up the always-present idle task.
pub fn init() {
    kprintln!("[SCHED] Initializing scheduler");
    // SAFETY (single-core): boot-time init, no other context runs yet.
    unsafe {
        let idle = core::ptr::addr_of_mut!(IDLE_TASK);
        (*idle) = Task::zeroed();
        (*idle).pid = NEXT_PID;
        NEXT_PID += 1;
        (*idle).state = TaskState::Running;
        (*idle).stack_phys = 0; // kernel boot stack, not PMM-managed
        (*idle).next = idle; // circular: points to itself
        copy_name(&mut (*idle).name, "idle");
        CURRENT = idle;
    }

    // Wire the scheduler into the interrupt + timer paths.
    exception::set_schedule_hook(schedule_hook);
    exception::timer::set_wake_sleepers(wake_sleepers);

    kprintln!("[SCHED] Initialized! Idle task registered");
}

/// Create a kernel-mode (EL1) task running `entry` on its own kernel stack.
/// Returns the new pid, or -1 on failure.
pub fn create_task(name: &str, entry: TaskEntry) -> i64 {
    let raw = heap::kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if raw.is_null() {
        crate::klib::uart::Uart.errorln("[SCHED] Failed to allocate task struct");
        return -1;
    }

    let stack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if stack_phys == 0 {
        crate::klib::uart::Uart.errorln("[SCHED] Failed to allocate task stack");
        heap::kfree(raw as *mut u8);
        return -1;
    }

    let stack_va = phys_to_virt(stack_phys);
    let stack_size = TASK_STACK_PAGES * PAGE_SIZE;
    let stack_top = stack_va + stack_size;

    unsafe {
        core::ptr::write(raw, Task::zeroed());
        core::ptr::write_bytes(stack_va as *mut u8, 0, stack_size as usize);

        // Initial context-switch frame at stack_top - 96:
        //   frame[0]  = x19 = entry fn
        //   frame[11] = x30 = task_trampoline (context_switch returns here)
        let frame = (stack_top - 96) as *mut u64;
        frame.add(0).write(entry as usize as u64);
        frame.add(11).write(task_trampoline as usize as u64);

        (*raw).sp = frame as u64;
        (*raw).pid = NEXT_PID;
        NEXT_PID += 1;
        (*raw).state = TaskState::Ready;
        (*raw).stack_phys = stack_phys;
        copy_name(&mut (*raw).name, name);

        // Tail-insert into the circular run queue (mask IRQs while mutating).
        let daif = irq_save();
        let current = CURRENT;
        let mut tail = current;
        while (*tail).next != current {
            tail = (*tail).next;
        }
        (*tail).next = raw;
        (*raw).next = current;
        irq_restore(daif);

        kprintln!(
            "[SCHED] Created task {} '{}' | stack: {:#x} - {:#x}",
            (*raw).pid,
            name,
            stack_va,
            stack_top
        );
        (*raw).pid as i64
    }
}

/// Pick the next READY task and switch to it. Called from the timer IRQ (via
/// the schedule hook) and voluntarily via `yield`/`sleep`/`task_exit`.
pub fn schedule() {
    reap();

    // SAFETY (single-core): callers are either in IRQ context (IRQs masked) or
    // mask IRQs around voluntary calls; the run queue is not concurrently
    // mutated. context_switch itself is the only place that changes SP.
    unsafe {
        let prev = CURRENT;
        let mut next = (*prev).next;

        while next != prev {
            if (*next).state == TaskState::Ready {
                break;
            }
            next = (*next).next;
        }

        if next == prev {
            return; // nothing else runnable; keep running prev
        }

        if (*prev).state == TaskState::Running {
            (*prev).state = TaskState::Ready;
        }

        // If prev died, unlink it and push onto the dead list for the reaper.
        if (*prev).state == TaskState::Dead {
            let mut p = prev;
            while (*p).next != prev {
                p = (*p).next;
            }
            (*p).next = (*prev).next;
            (*prev).next = DEAD_LIST;
            DEAD_LIST = prev;
        }

        (*next).state = TaskState::Running;
        CURRENT = next;

        context_switch(prev, next);
    }
}

/// IRQ-path schedule entry point registered with the exception dispatcher.
extern "C" fn schedule_hook() {
    schedule();
}

/// Voluntarily yield the CPU.
pub fn r#yield() {
    schedule();
}

/// Terminate the current task. Marks it dead and switches away; the idle loop's
/// `reap` frees its resources (it's still running on its own stack here).
///
/// Exported as `task_exit` because `task_trampoline` (switch.S) branches to it
/// when a kernel task's entry function returns.
#[no_mangle]
pub extern "C" fn task_exit() {
    // SAFETY (single-core): only the running task touches CURRENT here.
    unsafe {
        kprintln!(
            "[SCHED] Task {} '{}' exiting",
            (*CURRENT).pid,
            name_str(&(*CURRENT).name)
        );
        (*CURRENT).state = TaskState::Dead;
    }
    schedule();
}

/// Wake any sleeping tasks whose deadline has passed. Called from the timer IRQ.
pub fn wake_sleepers() {
    let now = exception::timer::get_ticks();
    // SAFETY (single-core): runs in timer IRQ context with IRQs masked.
    unsafe {
        let idle = core::ptr::addr_of_mut!(IDLE_TASK);
        let mut t = (*idle).next;
        while t != idle {
            if (*t).state == TaskState::Sleeping && now >= (*t).sleep_until {
                (*t).state = TaskState::Ready;
                (*t).sleep_until = 0;
            }
            t = (*t).next;
        }
    }
}

/// Sleep the current task for `ms` milliseconds (rounded up to one tick).
pub fn sleep_ms(ms: u64) {
    let ticks_needed = core::cmp::max(1, ms / exception::timer::TIMER_INTERVAL_MS);
    // SAFETY (single-core): only the running task updates its own fields.
    unsafe {
        (*CURRENT).sleep_until = exception::timer::get_ticks() + ticks_needed;
        (*CURRENT).state = TaskState::Sleeping;
        kprintln!(
            "[SCHED] Task {} '{}' sleeping for {} ms ({} ticks)",
            (*CURRENT).pid,
            name_str(&(*CURRENT).name),
            ms,
            ticks_needed
        );
    }
    schedule();
}

/// Free dead tasks' resources. Called from the idle loop and at schedule entry.
pub fn reap() {
    // SAFETY (single-core): dead_list is only touched with IRQs masked or from
    // the idle loop; mask here to be safe against a concurrent schedule().
    unsafe {
        loop {
            let dead = DEAD_LIST;
            if dead.is_null() {
                break;
            }
            DEAD_LIST = (*dead).next;

            kprintln!(
                "[SCHED] Reaping task {} '{}'",
                (*dead).pid,
                name_str(&(*dead).name)
            );

            if (*dead).stack_phys != 0 {
                pmm::free_pages((*dead).stack_phys, TASK_STACK_PAGES);
            }
            heap::kfree(dead as *mut u8);
        }
    }
}

/// The current task.
pub fn current() -> *mut Task {
    // SAFETY (single-core): read of a pointer-sized static.
    unsafe { CURRENT }
}

/// Head of the run queue (the idle task) for /proc/tasks iteration.
pub fn first_task() -> *mut Task {
    core::ptr::addr_of_mut!(IDLE_TASK)
}

pub fn state_name(s: TaskState) -> &'static str {
    match s {
        TaskState::Ready => "READY",
        TaskState::Running => "RUNNING",
        TaskState::Sleeping => "SLEEPING",
        TaskState::Dead => "DEAD",
    }
}

/// Interpret a NUL-padded task name buffer as a &str.
fn name_str(name: &[u8; 16]) -> &str {
    let len = name.iter().position(|&b| b == 0).unwrap_or(16);
    core::str::from_utf8(&name[..len]).unwrap_or("?")
}
