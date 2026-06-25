#![no_std]
#![no_main]
// The kernel exposes a deliberately complete, ported API surface (driver
// register maps, stats getters, string helpers) that isn't all wired to a
// caller yet, and the msr!/mrs! macros wrap asm in unsafe which the compiler
// flags as redundant inside larger unsafe blocks. Allow both crate-wide.
#![allow(dead_code)]
#![allow(unused_unsafe)]
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
mod elf;
mod exception;
mod fs;
mod mm;
mod mmio;
mod net;
mod panic;
mod pci;
mod print;
mod sched;
mod shell;
mod smp;
mod strings;
mod syscall;
mod sync;
mod virtio;
mod uart;

/// Timer-tick hook to wake sleeping tasks.
pub fn sched_wake_sleepers_hook() {
    sched::account_tick();
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

extern "C" fn netd() {
    let mut seq: u16 = 1;
    loop {
        sched::sleep_ms(5000);
        let ttl = net::ping(seq);
        if ttl >= 0 {
            kprintln!("[netd] ping seq={} reply ttl={}", seq, ttl);
        } else {
            kprintln!("[netd] ping seq={} no reply", seq);
        }
        seq = seq.wrapping_add(1);
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
    cpu::init();
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
    exception::timer::set_callback(|| {}); // silence default per-tick log

    // Scheduler + timer must be live before device bringup so the net RX
    // path's wfi-deadline waits make progress (timer ticks + net IRQs).
    sched::init();
    exception::timer::start(exception::timer::TIMER_INTERVAL_MS);
    // Seed a shared work queue (single-threaded), then bring up core 1 so both
    // cores drain it concurrently via a SpinLock (symmetric work distribution).
    smp::wq_seed(8000);
    smp::bringup();

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
    virtio::net::init();
    net::bringup();
    virtio::console::init();
    virtio::balloon::init();
    {
        let (a, t) = virtio::balloon::status();
        let inf = virtio::balloon::inflate(64);
        let (a2, _) = virtio::balloon::status();
        kprintln!("[boot] balloon: actual {}->{} (inflated {}), host_target {}", a, a2, inf, t);
    }

    // VFS + device nodes + fd table.
    fs::vfs::init();
    fs::devices::register();
    {
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/dev/rng");
        let mut b = [0u8; 8];
        let n = fs::vfs::fd_read(t, fd, &mut b);
        kprintln!("[boot] vfs: /dev/rng fd={} read {} bytes {:02x?}", fd, n, b);
        fs::vfs::fd_write(t, 1, b"    [vfs] hello via fd_write -> /dev/console (fd 1)
");
        fs::vfs::fd_close(t, fd);
        fs::vfs::fd_table_destroy(t);
    }

    // FAT32 mount under /mnt/fat32 + read a seed file through the VFS.
    fs::fat32::mount();
    let mnt = fs::vfs::create_node(fs::vfs::resolve("/"), "mnt", fs::vfs::VnodeType::Dir);
    fs::vfs::create_node(mnt, "fat32", fs::vfs::VnodeType::Dir);
    fs::fat32::vfs_mount("/mnt/fat32");
    fs::proc::mount();
    {
        let t = fs::vfs::fd_table_create();
        for path in ["/proc/uptime", "/proc/meminfo", "/proc/version"].iter() {
            let fd = fs::vfs::fd_open(t, path);
            if fd >= 0 {
                let mut b = [0u8; 256];
                let n = fs::vfs::fd_read(t, fd, &mut b);
                let txt = core::str::from_utf8(&b[..n.max(0) as usize]).unwrap_or("?");
                kprintln!("[boot] cat {}:", path);
                crate::kprint!("{}", txt);
                fs::vfs::fd_close(t, fd);
            }
        }
        fs::vfs::fd_table_destroy(t);
    }
    {
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/mnt/fat32/HELLO.TXT");
        if fd >= 0 {
            let mut b = [0u8; 128];
            let n = fs::vfs::fd_read(t, fd, &mut b);
            let txt = core::str::from_utf8(&b[..n.max(0) as usize]).unwrap_or("?");
            kprintln!("[boot] cat /mnt/fat32/HELLO.TXT ({} bytes):", n);
            crate::kprint!("{}", txt);
            fs::vfs::fd_close(t, fd);
        } else {
            kprintln!("[boot] could not open /mnt/fat32/HELLO.TXT");
        }
        fs::vfs::fd_table_destroy(t);
    }

    // Preemptive demo tasks.
    sched::create_task("netd", netd);
    sched::create_task("smpw", smp::smp_core0_worker);
    sched::create_user_task("user1");
    sched::create_task("shell", shell::shell_task);
    // Load an ELF binary from FAT32 and run it at EL0 (milestone 17).
    {
        use alloc::vec::Vec;
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/mnt/fat32/HELLO.ELF");
        if fd >= 0 {
            let mut data: Vec<u8> = Vec::new();
            let mut chunk = [0u8; 512];
            loop {
                let n = fs::vfs::fd_read(t, fd, &mut chunk);
                if n <= 0 { break; }
                data.extend_from_slice(&chunk[..n as usize]);
            }
            fs::vfs::fd_close(t, fd);
            kprintln!("[boot] loaded /mnt/fat32/HELLO.ELF ({} bytes)", data.len());
            sched::spawn_elf("hello", &data, &[]);
        }
        fs::vfs::fd_table_destroy(t);
    }

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
