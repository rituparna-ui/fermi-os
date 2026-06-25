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

/// A `Sync` `UnsafeCell` for kernel statics whose access discipline is enforced
/// by hand rather than by a lock — single-core state mutated only with IRQs
/// masked or during single-threaded boot. Using this instead of `static mut`
/// keeps the "I manage the aliasing" intent explicit in one audited place and
/// avoids the `static mut` reference footgun (a hard error in Rust 2024).
///
/// All access goes through `get()` (a raw pointer); every dereference still
/// needs `unsafe` and a `// SAFETY (single-core)` justification at the call
/// site naming the invariant that makes it sound.
#[repr(transparent)]
pub struct SyncUnsafeCell<T> {
    value: UnsafeCell<T>,
}

// SAFETY: the kernel guarantees exclusive access by convention (single core +
// IRQ masking). This mirrors the unstable std `SyncUnsafeCell`.
unsafe impl<T> Sync for SyncUnsafeCell<T> {}

impl<T> SyncUnsafeCell<T> {
    pub const fn new(value: T) -> Self {
        Self {
            value: UnsafeCell::new(value),
        }
    }

    /// Raw pointer to the contained value. Deref under the single-core invariant.
    #[inline(always)]
    pub fn get(&self) -> *mut T {
        self.value.get()
    }
}

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

/// A spin lock that also masks IRQs while held. Required for state shared
/// between a syscall/task context and an interrupt handler on a single core:
/// a plain `SpinLock` would deadlock if the IRQ fires while the lock is held
/// (the handler spins forever on a lock only the preempted task can release).
/// `lock()` saves DAIF + masks IRQs before acquiring; the guard restores DAIF
/// on drop.
pub struct SpinLockIrqSafe<T> {
    locked: AtomicBool,
    value: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for SpinLockIrqSafe<T> {}
unsafe impl<T: Send> Send for SpinLockIrqSafe<T> {}

impl<T> SpinLockIrqSafe<T> {
    pub const fn new(value: T) -> Self {
        Self {
            locked: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    pub fn lock(&self) -> SpinGuardIrqSafe<'_, T> {
        // Save DAIF and mask IRQs BEFORE acquiring, so an IRQ can't fire while
        // we hold the lock (which would deadlock on a single core).
        let daif: u64;
        unsafe {
            core::arch::asm!("mrs {}, daif", out(reg) daif, options(nomem, nostack));
            core::arch::asm!("msr daifset, #2", options(nomem, nostack));
        }
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuardIrqSafe { lock: self, daif }
    }
}

/// RAII guard for `SpinLockIrqSafe`: unlocks and restores DAIF on drop.
pub struct SpinGuardIrqSafe<'a, T> {
    lock: &'a SpinLockIrqSafe<T>,
    daif: u64,
}

impl<T> Deref for SpinGuardIrqSafe<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: holding the guard guarantees exclusive access.
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinGuardIrqSafe<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: holding the guard guarantees exclusive access.
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinGuardIrqSafe<'_, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
        // Restore the caller's prior IRQ-mask state.
        unsafe { core::arch::asm!("msr daif, {}", in(reg) self.daif, options(nomem, nostack)) };
    }
}
