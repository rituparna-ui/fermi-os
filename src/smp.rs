//! SMP: bring up a secondary core, enable the MMU on it, and run it in the
//! higher half (real high-VA kernel code with caches, atomics, and exception
//! vectors). This is the foundation for full SMP scheduling.

use crate::kprintln;
use crate::mm::mmu::KERNEL_VA_OFFSET;
use crate::sched::{self, Task, TASK_READY, TASK_RUNNING};
use crate::sync::SpinLock;
use crate::{mrs, msr};
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
static C1_TICKS: AtomicU64 = AtomicU64::new(0);

// --- Shared SpinLock-protected work queue drained by BOTH cores ---
const WQ_CAP: usize = 8192;
struct WorkQ {
    buf: [u32; WQ_CAP],
    head: usize,
    tail: usize,
    len: usize,
    enqueued: u64,
}
static WORKQ: SpinLock<WorkQ> = SpinLock::new(WorkQ {
    buf: [0; WQ_CAP],
    head: 0,
    tail: 0,
    len: 0,
    enqueued: 0,
});
static C0_CNT: AtomicU64 = AtomicU64::new(0);
static C0_SUM: AtomicU64 = AtomicU64::new(0);
static C1_CNT: AtomicU64 = AtomicU64::new(0);
static C1_SUM: AtomicU64 = AtomicU64::new(0);

fn wq_enqueue(id: u32) -> bool {
    let mut q = WORKQ.lock_irqsave();
    if q.len == WQ_CAP {
        return false;
    }
    let t = q.tail;
    q.buf[t] = id;
    q.tail = (t + 1) % WQ_CAP;
    q.len += 1;
    q.enqueued += 1;
    true
}
fn wq_pop() -> Option<u32> {
    let mut q = WORKQ.lock_irqsave();
    if q.len == 0 {
        return None;
    }
    let h = q.head;
    let v = q.buf[h];
    q.head = (h + 1) % WQ_CAP;
    q.len -= 1;
    Some(v)
}

/// Seed N jobs (ids 0..N). MUST be called single-threaded (before SMP bringup).
pub fn wq_seed(n: u32) {
    for i in 0..n {
        wq_enqueue(i);
    }
}

/// Process one job, attributing it to the calling core. Returns true if work
/// was done. The SpinLock guarantees each job is dequeued by exactly one core.
fn wq_consume(core0: bool) -> bool {
    if let Some(id) = wq_pop() {
        // Simulate per-job work OUTSIDE the lock so the two cores execute jobs
        // genuinely in parallel (the lock only guards the brief dequeue).
        let mut acc = id as u64;
        for _ in 0..60_000u64 {
            acc = acc.wrapping_add(1);
            core::hint::spin_loop();
        }
        core::hint::black_box(acc);
        if core0 {
            C0_CNT.fetch_add(1, Ordering::Relaxed);
            C0_SUM.fetch_add(id as u64, Ordering::Relaxed);
        } else {
            C1_CNT.fetch_add(1, Ordering::Relaxed);
            C1_SUM.fetch_add(id as u64, Ordering::Relaxed);
        }
        true
    } else {
        false
    }
}

/// Core 0 worker (a normal scheduled kernel task): drain the shared queue.
pub extern "C" fn smp_core0_worker() {
    loop {
        let mut did = 0u32;
        while wq_consume(true) {
            did += 1;
            if did > 64 {
                break; // yield periodically so other core-0 tasks run
            }
        }
        sched::sleep_ms(2);
    }
}

/// (c0_cnt, c1_cnt, c0_sum, c1_sum, enqueued, remaining)
pub fn wq_stats() -> (u64, u64, u64, u64, u64, usize) {
    let q = WORKQ.lock_irqsave();
    (
        C0_CNT.load(Ordering::Relaxed),
        C1_CNT.load(Ordering::Relaxed),
        C0_SUM.load(Ordering::Relaxed),
        C1_SUM.load(Ordering::Relaxed),
        q.enqueued,
        q.len,
    )
}
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

// Core-1 tasks are pure CPU loops with NO cooperative yield: only the
// per-core timer interrupt rotates them. If both counters advance, the
// secondary core is genuinely preempting.
// Core-1's two preemptive tasks are now work-queue consumers: each pops jobs
// from the shared SpinLock queue (attributing to core 1) and is preempted by
// core 1's timer between iterations.
extern "C" fn c1_task_a() {
    loop {
        C1_A_BEATS.fetch_add(1, Ordering::Relaxed);
        if pool_run_one(1) { continue; }       // symmetric pool first
        if !wq_consume(false) {
            for _ in 0..200_000u64 { core::hint::spin_loop(); }
        }
    }
}
extern "C" fn c1_task_b() {
    loop {
        C1_B_BEATS.fetch_add(1, Ordering::Relaxed);
        if !wq_consume(false) {
            for _ in 0..200_000u64 { core::hint::spin_loop(); }
        }
    }
}

/// Called from the IRQ handler on core 1 when the timer PPI fires: re-arm the
/// (banked) physical timer and account a core-1 tick.
pub fn c1_timer_tick() {
    let freq: u64 = mrs!(cntfrq_el0);
    let interval = freq / 100; // 10 ms
    let mut cval: u64 = mrs!(cntp_cval_el0);
    cval += interval;
    msr!(cntp_cval_el0, cval);
    C1_TICKS.fetch_add(1, Ordering::Relaxed);
    SECONDARY_BEATS.fetch_add(1, Ordering::Relaxed);
}

/// Called from the IRQ handler on core 1 after EOI: preempt to the next task.
pub fn c1_preempt() {
    if unsafe { C1_CUR } != 0 {
        c1_schedule();
    }
}

/// True if executing on a secondary core (affinity0 != 0).
#[inline]
pub fn is_secondary() -> bool {
    let mpidr: u64 = mrs!(mpidr_el1);
    (mpidr & 0xFF) != 0
}


// ===== Symmetric run-to-completion task pool (shared by BOTH cores) =====
// A shared SpinLock run queue of real Tasks. A core POPS a task (removing it,
// so no other core can take it), context_switches to it via a per-core
// scheduler context, the task runs to completion and switches back — never
// requeued mid-flight, so the "prev on two stacks" race cannot occur. Pool
// tasks run with IRQs masked (pool_trampoline never unmasks + pool_run_one
// masks around the nested switch) so neither core's preemptive scheduler is
// corrupted by the nested context switch.
const POOL_CAP: usize = 512;
struct PoolQ { buf: [u64; POOL_CAP], head: usize, tail: usize, len: usize, seeded: u64 }
static POOLQ: SpinLock<PoolQ> = SpinLock::new(PoolQ { buf: [0; POOL_CAP], head: 0, tail: 0, len: 0, seeded: 0 });
static mut SCHED_CTX: [u64; 2] = [0, 0];
static mut POOL_CUR: [u64; 2] = [0, 0];
static POOL_RAN: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];
static POOL_SUM: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];
static POOL_MIGRATIONS: AtomicU64 = AtomicU64::new(0);
static POOL_NEXT_PID: AtomicU64 = AtomicU64::new(1000);
static POOL_EXPECT: AtomicU64 = AtomicU64::new(0);

/// Allocate the per-core scheduler save-contexts. Call once, single-threaded.
pub fn pool_init() {
    unsafe {
        SCHED_CTX[0] = sched::alloc_bare_task() as u64;
        SCHED_CTX[1] = sched::alloc_bare_task() as u64;
    }
}

fn pool_push(t: *mut Task) {
    let mut q = POOLQ.lock_irqsave();
    if q.len < POOL_CAP { let i = q.tail; q.buf[i] = t as u64; q.tail = (i + 1) % POOL_CAP; q.len += 1; }
}
fn pool_pop() -> *mut Task {
    let mut q = POOLQ.lock_irqsave();
    if q.len == 0 { return core::ptr::null_mut(); }
    let i = q.head; let v = q.buf[i]; q.head = (i + 1) % POOL_CAP; q.len -= 1; v as *mut Task
}

/// Seed `k` pooled tasks (distinct pids) into the shared queue.
/// Seed `k` pooled tasks with globally-unique pids; returns the pid checksum
/// (sum of the new tasks' pids) so callers can verify exactly-once completion.
pub fn pool_seed(k: u64) -> u64 {
    let mut sum = 0u64;
    for _ in 0..k {
        let pid = POOL_NEXT_PID.fetch_add(1, Ordering::Relaxed);
        let t = sched::make_pool_task("pool", pid, pool_task_body);
        if !t.is_null() {
            sum += pid;
            POOL_EXPECT.fetch_add(pid, Ordering::Relaxed);
            pool_push(t);
            POOLQ.lock_irqsave().seeded += 1;
        }
    }
    sum
}

// ===== Parallel reduction over the symmetric pool: sum(1..=n) across cores =====
const PARSUM_CHUNKS: usize = 64;
static mut PARSUM_RANGE: [(u64, u64); PARSUM_CHUNKS] = [(0, 0); PARSUM_CHUNKS];
static PARSUM_BASE: AtomicU64 = AtomicU64::new(0);
static PARSUM_RESULT: AtomicU64 = AtomicU64::new(0);
static PARSUM_DONE: AtomicU64 = AtomicU64::new(0);

extern "C" fn parsum_task() {
    let core = (mrs!(mpidr_el1) & 0xff) as usize & 1;
    let pid = unsafe { (*(POOL_CUR[core] as *mut Task)).pid };
    let idx = (pid - PARSUM_BASE.load(Ordering::Relaxed)) as usize;
    if idx < PARSUM_CHUNKS {
        let (lo, hi) = unsafe { PARSUM_RANGE[idx] };
        let mut acc = 0u64;
        let mut i = lo;
        while i <= hi { acc = acc.wrapping_add(i); i += 1; }
        PARSUM_RESULT.fetch_add(acc, Ordering::Relaxed);
    }
    PARSUM_DONE.fetch_add(1, Ordering::Relaxed);
    smp_pool_return();
}

/// Compute sum(1..=n) by splitting it into chunks run as pooled tasks across
/// BOTH cores, then return (result, expected, chunks).
pub fn parsum(n: u64) -> (u64, u64, u64) {
    let chunks = if n < PARSUM_CHUNKS as u64 { n.max(1) } else { PARSUM_CHUNKS as u64 };
    PARSUM_RESULT.store(0, Ordering::Relaxed);
    PARSUM_DONE.store(0, Ordering::Relaxed);
    let base = POOL_NEXT_PID.load(Ordering::Relaxed);
    PARSUM_BASE.store(base, Ordering::Relaxed);
    let per = n / chunks;
    for c in 0..chunks {
        let lo = c * per + 1;
        let hi = if c == chunks - 1 { n } else { (c + 1) * per };
        unsafe { PARSUM_RANGE[c as usize] = (lo, hi); }
        let pid = POOL_NEXT_PID.fetch_add(1, Ordering::Relaxed);
        let t = sched::make_pool_task("parsum", pid, parsum_task);
        if !t.is_null() { pool_push(t); }
    }
    // Wait (bounded) for all chunk tasks to finish.
    let deadline = crate::exception::timer::uptime_ms() + 8000;
    while PARSUM_DONE.load(Ordering::Relaxed) < chunks {
        if crate::exception::timer::uptime_ms() >= deadline { break; }
        sched::sleep_ms(5);
    }
    (PARSUM_RESULT.load(Ordering::Relaxed), n * (n + 1) / 2, chunks)
}

extern "C" fn pool_task_body() {
    let first = (mrs!(mpidr_el1) & 0xff) as usize & 1;
    let mut migrated = false;
    // Several work rounds with cooperative yields between them. Each yield is
    // a safe migration point — the task may resume on the other core.
    for _ in 0..6 {
        for _ in 0..120_000u64 { core::hint::spin_loop(); }
        pool_yield();
        let now = (mrs!(mpidr_el1) & 0xff) as usize & 1;
        if now != first { migrated = true; }
    }
    if migrated { POOL_MIGRATIONS.fetch_add(1, Ordering::Relaxed); }
    // Account completion on whichever core finishes it.
    let core = (mrs!(mpidr_el1) & 0xff) as usize & 1;
    let pid = unsafe { (*(POOL_CUR[core] as *mut Task)).pid };
    POOL_RAN[core].fetch_add(1, Ordering::Relaxed);
    POOL_SUM[core].fetch_add(pid, Ordering::Relaxed);
    smp_pool_return();
}

/// Cooperative yield from inside a pool task: save this task's context and
/// return to the per-core scheduler, which will requeue it. Another core may
/// then pop and resume it — i.e. the task can migrate across cores. Safe: the
/// task is fully saved (by context_switch) before it is requeued, so it is
/// never runnable on two cores at once.
fn pool_yield() {
    let core = (mrs!(mpidr_el1) & 0xff) as usize & 1;
    unsafe {
        let t = POOL_CUR[core] as *mut Task;
        (*t).state = TASK_READY; // yielded, not finished
        let ctx = SCHED_CTX[core] as *mut Task;
        sched::raw_context_switch(t, ctx);
    }
    // resumed here (possibly on a different core) when popped + switched back
}

fn smp_pool_return() -> ! {
    let core = (mrs!(mpidr_el1) & 0xff) as usize & 1;
    unsafe {
        let t = POOL_CUR[core] as *mut Task;
        (*t).state = crate::sched::TASK_DEAD;
        let ctx = SCHED_CTX[core] as *mut Task;
        sched::raw_context_switch(t, ctx);
    }
    loop { unsafe { core::arch::asm!("wfi") } }
}

/// Run one pooled task on `core` non-preemptibly. Returns true if one ran.
pub fn pool_run_one(core: usize) -> bool {
    if unsafe { SCHED_CTX[core] } == 0 { return false; }
    let t = pool_pop();
    if t.is_null() { return false; }
    let daif: u64;
    unsafe {
        core::arch::asm!("mrs {}, daif", out(reg) daif, options(nomem, nostack));
        core::arch::asm!("msr daifset, #2", options(nomem, nostack));
        POOL_CUR[core] = t as u64;
        (*t).state = TASK_RUNNING;
        let ctx = SCHED_CTX[core] as *mut Task;
        sched::raw_context_switch(ctx, t); // run task; returns when it yields or finishes
        core::arch::asm!("msr daif, {}", in(reg) daif, options(nomem, nostack));
    }
    // After the switch: requeue if it yielded (READY) so either core can resume
    // it (migration); reclaim its stack + struct if it finished (DEAD).
    match unsafe { (*t).state } {
        TASK_READY => pool_push(t),
        crate::sched::TASK_DEAD => sched::free_pool_task(t),
        _ => {}
    }
    true
}

/// Core 0 worker: drain a few pooled tasks per scheduler slice, then yield.
pub extern "C" fn pool_sched_core0() {
    loop {
        let mut n = 0;
        while pool_run_one(0) { n += 1; if n >= 4 { break; } }
        sched::sleep_ms(2);
    }
}

/// Wait (bounded) until all seeded pool tasks have completed.
pub fn pool_join(timeout_ms: u64) -> bool {
    let deadline = crate::exception::timer::uptime_ms() + timeout_ms;
    loop {
        let (r0, r1, _, _, seeded, _, _, _) = pool_stats();
        if r0 + r1 >= seeded { return true; }
        if crate::exception::timer::uptime_ms() >= deadline { return false; }
        sched::sleep_ms(10);
    }
}

/// (ran0, ran1, sum0, sum1, seeded, remaining, migrations, expect_sum)
pub fn pool_stats() -> (u64, u64, u64, u64, u64, usize, u64, u64) {
    let q = POOLQ.lock_irqsave();
    (POOL_RAN[0].load(Ordering::Relaxed), POOL_RAN[1].load(Ordering::Relaxed),
     POOL_SUM[0].load(Ordering::Relaxed), POOL_SUM[1].load(Ordering::Relaxed),
     q.seeded, q.len, POOL_MIGRATIONS.load(Ordering::Relaxed), POOL_EXPECT.load(Ordering::Relaxed))
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
    // Bring up the GIC CPU interface + redistributor and the per-core timer
    // on core 1, then unmask IRQs so the timer preempts the tasks.
    crate::exception::gic::secondary_init();
    crate::exception::gic::secondary_enable_ppi(crate::exception::timer::TIMER_PPI_INTID);
    let freq: u64 = mrs!(cntfrq_el0);
    let now: u64 = mrs!(cntpct_el0);
    msr!(cntp_cval_el0, now + freq / 100); // 10 ms deadline
    msr!(cntp_ctl_el0, 1); // enable timer (IRQ still masked)

    SECONDARY_UP.store(1, Ordering::SeqCst);
    kprintln!("[SMP] core1 preemptive scheduling, 2 tasks (MPIDR={:#x})", mpidr);
    unsafe { core::arch::asm!("msr daifclr, #2") }; // unmask IRQs on core 1

    // Idle: wait for the timer; the IRQ handler drives c1 preemption.
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}

pub fn heartbeat() -> u64 {
    SECONDARY_BEATS.load(Ordering::Relaxed)
}
pub fn core1_ticks() -> u64 { C1_TICKS.load(Ordering::Relaxed) }
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
