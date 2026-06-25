//! Memory-mapped I/O accessors.
//!
//! Device registers are accessed through a global VA offset. While the MMU is
//! off (and after identity mapping) the offset is zero; once the kernel jumps
//! to the higher half it is set to `KERNEL_VA_OFFSET` so all existing physical
//! device addresses transparently route through TTBR1. This mirrors the C
//! `mmio_switch_to_upper()` design.

use core::sync::atomic::{AtomicUsize, Ordering};

/// Added to every MMIO address. Zero until `switch_to_upper()` is called.
static VA_OFFSET: AtomicUsize = AtomicUsize::new(0);

/// Route all subsequent MMIO through the higher-half kernel mapping.
pub fn switch_to_upper(offset: usize) {
    VA_OFFSET.store(offset, Ordering::SeqCst);
}

#[inline(always)]
fn va(addr: usize) -> usize {
    addr + VA_OFFSET.load(Ordering::Relaxed)
}

#[inline(always)]
pub fn write32(addr: usize, value: u32) {
    unsafe { core::ptr::write_volatile(va(addr) as *mut u32, value) };
}

#[inline(always)]
pub fn read32(addr: usize) -> u32 {
    unsafe { core::ptr::read_volatile(va(addr) as *const u32) }
}

#[inline(always)]
pub fn write16(addr: usize, value: u16) {
    unsafe { core::ptr::write_volatile(va(addr) as *mut u16, value) };
}

#[inline(always)]
pub fn read16(addr: usize) -> u16 {
    unsafe { core::ptr::read_volatile(va(addr) as *const u16) }
}

#[inline(always)]
pub fn write8(addr: usize, value: u8) {
    unsafe { core::ptr::write_volatile(va(addr) as *mut u8, value) };
}

#[inline(always)]
pub fn read8(addr: usize) -> u8 {
    unsafe { core::ptr::read_volatile(va(addr) as *const u8) }
}
