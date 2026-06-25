//! CPU/exception-level helpers.
//!
//! At this stage this mirrors the C `print_current_el` path. The full CPU
//! identification + PMU cycle-counter support (originally `cpu.c`) is ported
//! later, following the original commit progression.

use crate::klib::uart::Uart;
use crate::{mrs, msr};

/// Full-system data synchronization barrier (`dsb sy`). Required after MMIO
/// writes that change device state and before dependent reads — the canonical
/// barrier reused by every VirtIO driver.
#[inline(always)]
#[allow(dead_code)]
pub fn dsb_sy() {
    unsafe {
        core::arch::asm!("dsb sy", options(nostack, preserves_flags));
    }
}

/// Enable FP/SIMD at EL1 (CPACR_EL1.FPEN = 0b11).
///
/// The Rust compiler — like GCC for varargs — uses SIMD registers, including in
/// `core::fmt`. Without this, the first FP/SIMD instruction traps (ESR
/// `0x1FE0_0000`). Must run early in boot, before any formatting.
pub fn enable_fp_simd() {
    let mut cpacr = mrs!("cpacr_el1");
    cpacr |= 3 << 20;
    unsafe {
        msr!("cpacr_el1", cpacr);
        core::arch::asm!("isb");
    }
}

/// The current exception level, read from `CurrentEL[3:2]`.
pub fn current_el() -> u8 {
    let current_el = mrs!("CurrentEL");
    ((current_el >> 2) & 0b11) as u8
}

/// Human-readable name for an exception level.
pub fn el_name(el: u8) -> &'static str {
    match el {
        0 => "User Space",
        1 => "Kernel Space",
        2 => "Hyper Space",
        3 => "Secure Monitor/Firmware",
        _ => "Invalid Exception Level",
    }
}

/// Print the current exception level over the UART.
///
/// Uses the UART's direct string helpers rather than `kprintln!`. Before the
/// MMU is enabled, RAM is treated as Device memory (strongly-ordered), so the
/// unaligned 2-byte accesses that `core::fmt`'s integer formatting emits fault
/// — and no exception vectors are installed yet. The aligned `puts`/`putc`
/// path is safe; `kprintln!`/`core::fmt` become usable once the MMU maps RAM as
/// Normal cacheable memory.
pub fn print_current_el() {
    let el = current_el();
    let uart = Uart;
    uart.puts("Current Exception Level: ");
    uart.println(el_name(el));
}
