//! ARM Generic Timer — periodic tick via the physical timer (PPI INTID 30).
//!
//! Uses absolute deadlines (CNTP_CVAL_EL0) re-armed each IRQ so latency does not
//! accumulate. The tick drives the scheduler: `handle_irq` wakes sleeping tasks
//! (via a registered hook) and invokes an optional tick callback.

use crate::exception::gic;
use crate::klib::sync::SpinLockIrqSafe;
use crate::kprintln;
use crate::{mrs, msr};

/// Private-peripheral interrupt ID for the EL1 physical timer on QEMU virt.
pub const TIMER_PPI_INTID: u32 = 30;
/// Default tick interval: 10 ms for fine-grained sleep.
pub const TIMER_INTERVAL_MS: u64 = 10;

struct Timer {
    freq: u64,
    interval: u64,
    tick_count: u64,
    callback: Option<fn()>,
    /// Hook the scheduler registers to wake sleeping tasks each tick.
    wake_sleepers: Option<fn()>,
}

// IRQ-safe: TIMER is locked both from task context (sleep_ms / uptime_ms via a
// syscall, IRQs unmasked) and from the timer IRQ handler. A plain SpinLock here
// deadlocks if a tick lands while a syscall holds the lock.
static TIMER: SpinLockIrqSafe<Timer> = SpinLockIrqSafe::new(Timer {
    freq: 0,
    interval: 0,
    tick_count: 0,
    callback: None,
    wake_sleepers: None,
});

/// Initialize the timer: read CNTFRQ and enable its PPI in the GIC.
pub fn init() {
    kprintln!("[TIMER] Initializing hardware timer");
    let freq = mrs!("cntfrq_el0");
    TIMER.lock().freq = freq;
    kprintln!(
        "[TIMER] Frequency: {} Hz ({} MHz)",
        freq,
        freq / 1_000_000
    );

    gic::enable_irq(TIMER_PPI_INTID);
    kprintln!("[TIMER] Initialized!");
}

/// Start periodic ticks at `interval_ms`.
pub fn start(interval_ms: u64) {
    let interval;
    {
        let mut t = TIMER.lock();
        if t.freq == 0 {
            crate::klib::uart::Uart.errorln("[TIMER] Not initialized! Call init() first");
            return;
        }
        t.interval = t.freq * interval_ms / 1000;
        t.tick_count = 0;
        interval = t.interval;
    }

    kprintln!(
        "[TIMER] Starting with interval: {} ms ({} ticks)",
        interval_ms,
        interval
    );

    // Absolute-deadline arming: CVAL = now + interval, then enable.
    let now = mrs!("cntpct_el0");
    unsafe {
        msr!("cntp_cval_el0", now + interval);
        msr!("cntp_ctl_el0", 1);
    }
    kprintln!("[TIMER] Started!");
}

/// Stop the timer.
pub fn stop() {
    unsafe {
        msr!("cntp_ctl_el0", 0);
    }
    let ticks = TIMER.lock().tick_count;
    kprintln!("[TIMER] Stopped after {} ticks", ticks);
}

/// Timer IRQ handler: advance the deadline, wake sleepers, run the callback.
pub fn handle_irq() {
    let (interval, callback, wake, count) = {
        let mut t = TIMER.lock();
        t.tick_count += 1;
        (t.interval, t.callback, t.wake_sleepers, t.tick_count)
    };

    // Re-arm by advancing the absolute deadline (bounded drift).
    let mut cval = mrs!("cntp_cval_el0");
    cval += interval;
    unsafe {
        msr!("cntp_cval_el0", cval);
    }

    if let Some(wake) = wake {
        wake();
    }

    if let Some(cb) = callback {
        cb();
    } else if count % 100 == 0 {
        // 1 s at a 10 ms interval; avoid per-tick spam.
        kprintln!("[TIMER] tick {}", count);
    }
}

pub fn set_callback(cb: fn()) {
    TIMER.lock().callback = Some(cb);
}

/// Register the scheduler's wake-sleepers hook (called once when sched starts).
pub fn set_wake_sleepers(hook: fn()) {
    TIMER.lock().wake_sleepers = Some(hook);
}

pub fn get_frequency() -> u64 {
    TIMER.lock().freq
}

/// Physical counter (CNTPCT_EL0).
pub fn get_count() -> u64 {
    mrs!("cntpct_el0")
}

pub fn get_ticks() -> u64 {
    TIMER.lock().tick_count
}

pub fn uptime_ms() -> u64 {
    TIMER.lock().tick_count * TIMER_INTERVAL_MS
}

pub fn uptime_seconds() -> u64 {
    (TIMER.lock().tick_count * TIMER_INTERVAL_MS) / 1000
}
