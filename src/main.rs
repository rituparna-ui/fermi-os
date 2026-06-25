//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]

mod arch;
mod panic;

use core::arch::global_asm;

// Pull in the assembly entry point. LLVM's integrated assembler handles the
// GNU-syntax `.S` file, so no external `as`/binutils is required.
global_asm!(include_str!("arch/boot.S"));

/// Rust entry point, called from `_start` (see `arch/boot.S`) with the stack
/// already set up. Never returns.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    // The UART driver lands in the next step; for now prove the boot path by
    // writing a banner directly to the PL011 data register.
    let uart = 0x0900_0000 as *mut u8;
    for &b in b"Fermi OS (Rust) - Booting Up...\n" {
        unsafe { core::ptr::write_volatile(uart, b) };
    }

    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}
