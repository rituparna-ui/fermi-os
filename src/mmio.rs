//! Memory-mapped I/O helpers.
//!
//! Mirrors the original `src/lib/mmio/mmio.c`. All device accesses are routed
//! through a global VA offset which is 0 while the kernel runs identity-mapped
//! and becomes `KERNEL_VA_OFFSET` once the MMU promotes us to the higher half.

use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{AtomicUsize, Ordering};

static MMIO_VA_OFFSET: AtomicUsize = AtomicUsize::new(0);

/// Route subsequent MMIO through the higher-half VA window.
pub fn switch_to_upper(offset: usize) {
    MMIO_VA_OFFSET.store(offset, Ordering::SeqCst);
}

#[inline(always)]
fn off() -> usize {
    MMIO_VA_OFFSET.load(Ordering::Relaxed)
}

#[inline(always)]
pub fn write32(addr: usize, value: u32) {
    unsafe { write_volatile((addr + off()) as *mut u32, value) }
}

#[inline(always)]
pub fn read32(addr: usize) -> u32 {
    unsafe { read_volatile((addr + off()) as *const u32) }
}

#[inline(always)]
pub fn write16(addr: usize, value: u16) {
    unsafe { write_volatile((addr + off()) as *mut u16, value) }
}

#[inline(always)]
pub fn read16(addr: usize) -> u16 {
    unsafe { read_volatile((addr + off()) as *const u16) }
}

#[inline(always)]
pub fn write8(addr: usize, value: u8) {
    unsafe { write_volatile((addr + off()) as *mut u8, value) }
}

#[inline(always)]
pub fn read8(addr: usize) -> u8 {
    unsafe { read_volatile((addr + off()) as *const u8) }
}
