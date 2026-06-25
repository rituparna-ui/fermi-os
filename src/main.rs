//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]

#[macro_use]
mod klib;
#[macro_use]
mod arch;
mod mm;
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

    // Pre-MMU: RAM is Device memory, so avoid core::fmt (it emits unaligned
    // accesses that fault with no vectors installed). Plain string output via
    // the UART is safe. Once the MMU maps RAM as Normal memory, kprintln! is
    // usable everywhere.
    klib::uart::Uart.println("Fermi OS (Rust) - Booting Up...");
    arch::cpu::print_current_el();

    // Physical memory manager (pre-MMU, identity mapped).
    mm::pmm::init(mm::pmm::MEM_START, mm::pmm::MEM_SIZE);
    mm::pmm::print_info();

    // Quick sanity check: allocate, then free, a single page.
    let p = mm::pmm::allocate_page();
    let uart = klib::uart::Uart;
    uart.puts("[PMM][TEST] allocated page at ");
    uart.puthex(p);
    uart.putc(b'\n');
    mm::pmm::free_page(p);
    uart.puts("[PMM][TEST] freed; free pages now ");
    uart.putdec(mm::pmm::free_pages_count());
    uart.putc(b'\n');

    // Echo received bytes, mirroring the original early kernel loop.
    loop {
        let c = uart.getc();
        uart.putc(c);
    }
}
