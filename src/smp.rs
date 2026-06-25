//! SMP: bring up a secondary core, enable the MMU on it, and run it in the
//! higher half (real high-VA kernel code with caches, atomics, and exception
//! vectors). This is the foundation for full SMP scheduling.

use crate::kprintln;
use crate::mm::mmu::KERNEL_VA_OFFSET;
use crate::sched::{self, Task, TASK_READY, TASK_RUNNING};
use crate::mrs;
use core::arch::global_asm;
use core::sync::atomic::{AtomicU64, Ordering};

global_asm!(include_str!("smp.S"));

extern "C" {
    fn secondary_start();
}

#[repr(C, align(64))]
struct SecStack([u8; 16384]);
#[no_mangle]
static mut SECONDARY_STACK: SecStack = SecStack([0; 16384]);
core::arch::global_asm!(
    ".globl secondary_stack_top\n.set secondary_stack_top, SECONDARY_STACK + 16384"
);

static SECONDARY_UP: AtomicU64 = AtomicU64::new(0);
static SECONDARY_MPIDR: AtomicU64 = AtomicU64::new(0);
static SECONDARY_BEATS: AtomicU64 = AtomicU64::new(0);
static C1_A_BEATS: AtomicU64 = AtomicU64::new(0);
static C1_B_BEATS: AtomicU64 = AtomicU64::new(0);
// Core-1's "current task" pointer (only core 1 touches this).
static mut C1_CUR: u64 = 0;

const PSCI_CPU_ON: u64 = 0xC400_0003;

/// Pre-MMU secondary init: enable the MMU using the primary's page tables.
#[no_mangle]
pub extern "C" fn rust_secondary_early() {
    crate::mm::mmu::enable_on_this_core();
}

/// Higher-half secondary entry (MMU on, high VA). Installs vectors, then runs
/// an atomic heartbeat — proving a second core executes real kernel code with
/// caches and atomics.
/// Core-1 cooperative scheduler: switch to the next task in the ring.
fn c1_schedule() {
    unsafe {
        let prev = C1_CUR as *mut Task;
        let next = (*prev).next as *mut Task;
        C1_CUR = next as u64;
        if (*prev).state == TASK_RUNNING {
            (*prev).state = TASK_READY;
        }
        (*next).state = TASK_RUNNING;
        sched::raw_context_switch(prev, next);
    }
}

/// Yield from a core-1 task back to the core-1 scheduler.
fn c1_yield() {
    c1_schedule();
}

extern "C" fn c1_task_a() {
    loop {
        C1_A_BEATS.fetch_add(1, Ordering::Relaxed);
        for _ in 0..2_000_000u64 { core::hint::spin_loop(); }
        c1_yield();
    }
}
extern "C" fn c1_task_b() {
    loop {
        C1_B_BEATS.fetch_add(1, Ordering::Relaxed);
        for _ in 0..2_000_000u64 { core::hint::spin_loop(); }
        c1_yield();
    }
}

#[no_mangle]
pub extern "C" fn rust_secondary_high() -> ! {
    crate::exception::set_vbar_current();
    let mpidr: u64 = mrs!(mpidr_el1);
    SECONDARY_MPIDR.store(mpidr, Ordering::SeqCst);

    // Build core-1's own task set (idle + 2 tasks). All allocation happens
    // now, while core 0 waits on SECONDARY_UP, so the unlocked PMM is not
    // accessed concurrently. After UP=1 core 1 only does atomic work.
    let idle = sched::alloc_bare_task();
    let a = sched::make_kernel_task("c1a", 100, c1_task_a);
    let b = sched::make_kernel_task("c1b", 101, c1_task_b);
    unsafe {
        (*idle).next = a as u64;
        (*a).next = b as u64;
        (*b).next = idle as u64;
        (*idle).state = TASK_RUNNING;
        C1_CUR = idle as u64;
    }
    SECONDARY_UP.store(1, Ordering::SeqCst);
    kprintln!("[SMP] core1 scheduling 2 tasks (MPIDR={:#x})", mpidr);

    // Idle loop: cycle the scheduler so the two tasks make progress.
    loop {
        SECONDARY_BEATS.fetch_add(1, Ordering::Relaxed);
        c1_schedule();
    }
}

pub fn heartbeat() -> u64 {
    SECONDARY_BEATS.load(Ordering::Relaxed)
}
pub fn task_beats() -> (u64, u64) {
    (C1_A_BEATS.load(Ordering::Relaxed), C1_B_BEATS.load(Ordering::Relaxed))
}
pub fn secondary_mpidr() -> u64 {
    SECONDARY_MPIDR.load(Ordering::Relaxed)
}
pub fn secondary_online() -> bool {
    SECONDARY_UP.load(Ordering::Relaxed) != 0
}

/// Bring up core 1 via PSCI CPU_ON and wait for it to reach the higher half.
pub fn bringup() {
    let entry = (secondary_start as usize as u64).wrapping_sub(KERNEL_VA_OFFSET);
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "hvc #0",
            inout("x0") PSCI_CPU_ON => ret,
            in("x1") 1u64,
            in("x2") entry,
            in("x3") 0u64,
            options(nomem, nostack)
        );
    }
    if ret != 0 {
        kprintln!("[SMP] CPU_ON failed: {} (single-core?)", ret as i64);
        return;
    }
    for _ in 0..50_000_000u64 {
        if secondary_online() {
            kprintln!("[SMP] secondary online MPIDR={:#x}", secondary_mpidr());
            return;
        }
        core::hint::spin_loop();
    }
    kprintln!("[SMP] secondary did not come online");
}
