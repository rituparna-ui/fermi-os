//! Minimal synchronization primitives for the kernel.
//!
//! A ticket-free test-and-set `SpinLock` is enough for a single-core kernel
//! where the only contention is between the main thread and interrupt context.
//! It is declared unconditionally `Sync` (kernel convention): the kernel owns
//! all its statics and is responsible for not constructing aliasing that the
//! type system would otherwise forbid.

use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, Ordering};

/// A spin lock guarding a value of type `T`.
pub struct SpinLock<T> {
    locked: AtomicBool,
    value: UnsafeCell<T>,
}

// The kernel is responsible for ensuring soundness of shared access; the lock
// provides the mutual exclusion. This mirrors how C kernels treat globals.
unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

impl<T> SpinLock<T> {
    pub const fn new(value: T) -> Self {
        Self {
            locked: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    /// Acquire the lock, spinning until it is free. Returns a guard that
    /// releases on drop.
    pub fn lock(&self) -> SpinGuard<'_, T> {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            // Hint the core that we are spinning.
            core::hint::spin_loop();
        }
        SpinGuard { lock: self }
    }
}

/// RAII guard that unlocks its `SpinLock` when dropped.
pub struct SpinGuard<'a, T> {
    lock: &'a SpinLock<T>,
}

impl<T> Deref for SpinGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: holding the guard guarantees exclusive access.
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: holding the guard guarantees exclusive access.
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
    }
}
