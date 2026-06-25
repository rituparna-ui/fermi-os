#![no_std]
#![no_main]
//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel, pure Rust + assembly.
//!
//! This is a progressive port of the original C+asm Fermi OS, built up in the
//! same subsystem order as the original git history.

extern crate alloc;

use core::arch::global_asm;
use core::panic::PanicInfo;

// Boot stub: sets up the stack, zeroes BSS, and calls `rust_main`.
global_asm!(include_str!("boot.S"));

mod cpu;
mod exception;
mod mm;
mod mmio;
mod panic;
mod print;
mod sync;
mod uart;

/// Timer-tick hook to wake sleeping tasks. No-op until the scheduler lands.
pub fn sched_wake_sleepers_hook() {}

/// Post-IRQ scheduling hook. No-op until the scheduler lands.
pub fn schedule_hook() {}

/// Kernel entry point, called from `boot.S` after low-level setup.
#[no_mangle]
pub extern "C" fn rust_main() -> ! {
    uart::init();
    uart::putc(b'\n');
    uart::println("================================");
    uart::println("  Fermi OS (Rust) is booting");
    uart::println("================================");
    cpu::print_current_el();

    // Physical memory manager.
    mm::pmm::init(mm::pmm::MEM_START, mm::pmm::MEM_SIZE);
    mm::pmm::print_info();

    // PMM smoke test: allocate a few pages, free one, re-allocate.
    let a = mm::pmm::allocate_page();
    let b = mm::pmm::allocate_page();
    uart::log_hex("[TEST] alloc a=", a);
    uart::log_hex("[TEST] alloc b=", b);
    mm::pmm::free_page(a);
    let c = mm::pmm::allocate_page();
    uart::log_hex("[TEST] freed a, re-alloc c= (expect==a) ", c);
    mm::pmm::free_page(b);
    mm::pmm::free_page(c);

    let big = mm::pmm::allocate_pages(8);
    uart::log_hex("[TEST] alloc 8 contiguous pages at ", big);
    mm::pmm::free_pages(big, 8);

    // Enable the MMU. After this, RAM is Normal cacheable memory and
    // core::fmt (kprintln!) + SpinLock are safe to use.
    let l1_lo = mm::mmu::init();
    exception::init();
    mm::mmu::run_tests(l1_lo);
    kprintln!("[boot] MMU on; kprintln! now safe. EL={}", cpu::current_el());

    // Kernel heap (backs the global allocator: Vec/Box/String).
    mm::heap::init();
    mm::heap::run_tests();
    {
        use alloc::vec::Vec;
        let mut v: Vec<u64> = Vec::new();
        for i in 0..8 { v.push(i * i); }
        kprintln!("[boot] alloc::Vec works: {:?}", v.as_slice());
    }

    // GICv3 + generic timer (10ms tick). Enables IRQs.
    exception::gic::init();
    exception::timer::init();
    exception::timer::start(exception::timer::TIMER_INTERVAL_MS);
    kprintln!("[boot] timer running; waiting for ticks (1 line/sec)...");
    uart::puts("UART base: ");
    uart::puthex(uart::UART_BASE as u64);
    uart::putc(b'\n');

    uart::println("[boot] reached idle loop");
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    uart::putc(b'\n');
    if let Some(loc) = info.location() {
        kprintln!("[panic] at {}:{}", loc.file(), loc.line());
    }
    kprintln!("[panic] {}", info.message());
    panic::kernel_panic("Rust panic");
}
