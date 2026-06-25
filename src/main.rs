//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]

mod arch;
#[macro_use]
mod klib;
mod panic;

use core::arch::global_asm;

// Pull in the assembly entry point. LLVM's integrated assembler handles the
// GNU-syntax `.S` file, so no external `as`/binutils is required.
global_asm!(include_str!("arch/boot.S"));

/// Rust entry point, called from `_start` (see `arch/boot.S`) with the stack
/// already set up. Never returns.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    klib::uart::init();

    kprintln!("Fermi OS (Rust) - Booting Up...");

    // Echo received bytes, mirroring the original early kernel loop.
    let uart = klib::uart::Uart;
    loop {
        let c = uart.getc();
        uart.putc(c);
    }
}
