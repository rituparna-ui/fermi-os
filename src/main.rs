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
mod pci;
mod print;
mod sched;
mod strings;
mod syscall;
mod sync;
mod virtio;
mod uart;

/// Timer-tick hook to wake sleeping tasks.
pub fn sched_wake_sleepers_hook() {
    sched::wake_sleepers();
}

/// Post-IRQ scheduling hook: round-robin preemption.
pub fn schedule_hook() {
    sched::schedule();
}

extern "C" fn task_a() {
    for i in 0..5 {
        kprintln!("    [task_a] iteration {}", i);
        sched::sleep_ms(300);
    }
}

extern "C" fn task_b() {
    for i in 0..4 {
        kprintln!("    [task_b] iteration {}", i);
        sched::sleep_ms(500);
    }
}

/// Pre-MMU init, called from `boot.S` while running at the physical address.
/// Sets up UART + PMM and enables the MMU, then returns so boot.S can jump to
/// the upper half. Rust static accesses here resolve to physical addresses
/// (PC-relative) because PC is physical.
#[no_mangle]
pub extern "C" fn early_init() {
    uart::init();
    uart::putc(b'\n');
    uart::println("================================");
    uart::println("  Fermi OS (Rust) is booting");
    uart::println("================================");

    // Physical memory manager (bitmap placed at physical __kernel_end).
    mm::pmm::init(mm::pmm::MEM_START, mm::pmm::MEM_SIZE);
    mm::pmm::print_info();

    // Build page tables and enable the MMU (identity-low + upper-half).
    let _l1_lo = mm::mmu::init();
    uart::println("[boot] early_init done; jumping to higher half");
}

/// Higher-half kernel entry, branched to from `boot.S` at the upper-half VA.
#[no_mangle]
pub extern "C" fn kernel_main() -> ! {
    // Route MMIO through the upper half and relocate the PMM bitmap before any
    // allocation, so everything stays mapped once TTBR0 is swapped per task.
    mmio::switch_to_upper(mm::mmu::KERNEL_VA_OFFSET as usize);
    mm::pmm::relocate_upper();

    uart::println("[boot] running in higher half (TTBR1)");
    cpu::print_current_el();
    kprintln!("[boot] kernel VA base in use; EL={}", cpu::current_el());

    // Exception vectors (VBAR now resolves to the upper-half VA).
    exception::init();
    mm::mmu::run_tests(0);

    // Kernel heap (backs the global allocator: Vec/Box/String).
    mm::heap::init();
    mm::heap::run_tests();
    {
        use alloc::vec::Vec;
        let mut v: Vec<u64> = Vec::new();
        for i in 0..8 {
            v.push(i * i);
        }
        kprintln!("[boot] alloc::Vec works: {:?}", v.as_slice());
    }

    // GICv3 + generic timer (10ms tick). Enables IRQs.
    exception::gic::init();
    exception::timer::init();

    // PCI enumeration + VirtIO RNG.
    pci::enumerate_bus();
    virtio::rng::init();
    {
        let mut buf = [0u8; 16];
        let n = virtio::rng::read(&mut buf);
        kprintln!("[boot] rng_read {} bytes: {:02x?}", n, &buf[..]);
    }
    virtio::blk::init();
    if virtio::blk::is_ready() {
        let mut sec = alloc::vec![0u8; 512];
        if virtio::blk::read(0, &mut sec) {
            let marker = core::str::from_utf8(&sec[..16]).unwrap_or("?");
            kprintln!("[boot] blk sector0[0..16] = {:?}", marker);
        }
    }

    // Scheduler + a couple of preemptive EL1 demo tasks.
    sched::init();
    sched::create_task("task_a", task_a);
    sched::create_user_task("user1");
    exception::timer::start(exception::timer::TIMER_INTERVAL_MS);
    kprintln!("[boot] scheduler running; idle loop reaping dead tasks");

    loop {
        sched::reap();
        unsafe { core::arch::asm!("wfi") };
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
