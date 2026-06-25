//! ARM generic timer — periodic tick via the physical timer + GIC PPI.
//!
//! Port of `src/exception/timer/timer.c`. Uses absolute deadlines (CNTP_CVAL)
//! for drift-free ticking. The scheduler wake-sleepers hook is added at the
//! scheduler milestone.

use crate::exception::gic;
use crate::kprintln;
use crate::sync::Racy;
use crate::uart;

pub const TIMER_PPI_INTID: u32 = 30;
pub const TIMER_INTERVAL_MS: u64 = 10;

struct Timer {
    freq: u64,
    interval: u64,
    ticks: u64,
    callback: Option<fn()>,
}

static TIMER: Racy<Timer> = Racy::new(Timer {
    freq: 0,
    interval: 0,
    ticks: 0,
    callback: None,
});

pub fn init() {
    uart::println("[TIMER] Initializing hardware timer");
    let freq: u64 = crate::mrs!(cntfrq_el0);
    let t = unsafe { TIMER.get() };
    t.freq = freq;
    kprintln!(
        "[TIMER] Frequency: {} Hz ({} MHz)",
        freq,
        freq / 1_000_000
    );
    gic::enable_irq(TIMER_PPI_INTID);
    uart::println("[TIMER] Initialized!");
}

pub fn start(interval_ms: u64) {
    let t = unsafe { TIMER.get() };
    if t.freq == 0 {
        uart::errorln("[TIMER] Not initialized! Call timer::init() first");
        return;
    }
    t.interval = t.freq * interval_ms / 1000;
    t.ticks = 0;
    kprintln!(
        "[TIMER] Starting with interval: {} ms ({} ticks)",
        interval_ms,
        t.interval
    );
    let now: u64 = crate::mrs!(cntpct_el0);
    crate::msr!(cntp_cval_el0, now + t.interval); // absolute deadline
    crate::msr!(cntp_ctl_el0, 1); // enable
    uart::println("[TIMER] Started!");
}

pub fn stop() {
    crate::msr!(cntp_ctl_el0, 0);
    let t = unsafe { TIMER.get() };
    kprintln!("[TIMER] Stopped after {} ticks", t.ticks);
}

pub fn handle_irq() {
    let t = unsafe { TIMER.get() };
    t.ticks += 1;

    // Re-arm by advancing the absolute deadline (drift-free).
    let mut cval: u64 = crate::mrs!(cntp_cval_el0);
    cval += t.interval;
    crate::msr!(cntp_cval_el0, cval);

    // Wake sleeping tasks (wired at the scheduler milestone).
    crate::sched_wake_sleepers_hook();

    if let Some(cb) = t.callback {
        cb();
    } else if t.ticks % 100 == 0 {
        kprintln!("[TIMER] tick {}", t.ticks);
    }
}

pub fn set_callback(cb: fn()) {
    unsafe { TIMER.get() }.callback = Some(cb);
}

pub fn get_frequency() -> u64 {
    unsafe { TIMER.get() }.freq
}

pub fn get_count() -> u64 {
    crate::mrs!(cntpct_el0)
}

pub fn get_ticks() -> u64 {
    unsafe { TIMER.get() }.ticks
}

pub fn uptime_ms() -> u64 {
    unsafe { TIMER.get() }.ticks * TIMER_INTERVAL_MS
}

pub fn uptime_seconds() -> u64 {
    uptime_ms() / 1000
}
