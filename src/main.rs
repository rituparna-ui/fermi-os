#![no_std]
#![no_main]
//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel, pure Rust + assembly.
//!
//! This is a progressive port of the original C+asm Fermi OS, built up in the
//! same subsystem order as the original git history.

use core::arch::global_asm;
use core::panic::PanicInfo;

// Boot stub: sets up the stack, zeroes BSS, and calls `rust_main`.
global_asm!(include_str!("boot.S"));

mod cpu;
mod mmio;
mod uart;

/// Kernel entry point, called from `boot.S` after low-level setup.
#[no_mangle]
pub extern "C" fn rust_main() -> ! {
    uart::init();
    uart::putc(b'\n');
    uart::println("================================");
    uart::println("  Fermi OS (Rust) is booting");
    uart::println("================================");
    cpu::print_current_el();
    uart::puts("UART base: ");
    uart::puthex(uart::UART_BASE as u64);
    uart::putc(b'\n');

    uart::println("[boot] reached idle loop");
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    uart::putc(b'\n');
    uart::errorln("KERNEL PANIC");
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}
