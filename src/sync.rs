//! Minimal spinlock for guarding shared kernel state.
//!
//! Fermi is currently single-core, but interrupts can preempt; a spinlock
//! gives us a `Sync` cell with interior mutability and a clean borrow API
//! that we can later harden (IRQ-save) without touching call sites.

use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, Ordering};

/// Non-atomic global cell for state touched **before the MMU is enabled**.
///
/// Exclusive load/store instructions (LDXR/STXR), which back atomic
/// read-modify-write ops like `SpinLock`'s `compare_exchange`, are not
/// architecturally supported on Device memory — and all RAM is Device-typed
/// while the MMU is off. So early subsystems (PMM, page-table setup) use this
/// lock-free cell instead, exactly like the original C globals. Safe because
/// the kernel is single-core with interrupts masked during early boot.
pub struct Racy<T> {
    data: UnsafeCell<T>,
}

unsafe impl<T> Sync for Racy<T> {}

impl<T> Racy<T> {
    pub const fn new(v: T) -> Self {
        Self {
            data: UnsafeCell::new(v),
        }
    }

    /// Obtain a mutable reference. Caller must ensure no aliasing.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get(&self) -> &mut T {
        &mut *self.data.get()
    }
}


pub struct SpinLock<T> {
    locked: AtomicBool,
    data: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

pub struct SpinGuard<'a, T> {
    lock: &'a SpinLock<T>,
}

impl<T> SpinLock<T> {
    pub const fn new(data: T) -> Self {
        Self {
            locked: AtomicBool::new(false),
            data: UnsafeCell::new(data),
        }
    }

    pub fn lock(&self) -> SpinGuard<'_, T> {
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard { lock: self }
    }
}

impl<'a, T> Deref for SpinGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> DerefMut for SpinGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<'a, T> Drop for SpinGuard<'a, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
    }
}
