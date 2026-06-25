//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]

extern crate alloc;

#[macro_use]
mod klib;
#[macro_use]
mod arch;
mod drivers;
mod exception;
mod fs;
mod mm;
mod panic;
mod sched;

use core::arch::global_asm;

// Pull in the assembly entry point. LLVM's integrated assembler handles the
// GNU-syntax `.S` file, so no external `as`/binutils is required.
global_asm!(include_str!("arch/boot.S"));

/// Early init — runs physically (PC == PA) with the MMU off, called from
/// `_start`. Brings up the UART, physical memory manager, and the MMU. After it
/// returns, `boot.S` switches to the upper-half stack and branches to `kmain`.
///
/// All logging here uses the UART's aligned helpers: RAM is Device memory until
/// the MMU maps it Normal, so `core::fmt` would fault.
#[no_mangle]
pub extern "C" fn early_init() {
    // Rust uses SIMD (incl. in core::fmt); enable it before anything formats.
    arch::cpu::enable_fp_simd();

    // IMPORTANT: everything before `mmu::init` runs at the physical PC with the
    // MMU off, so it must be position-independent. Code (PC-relative `adr`) and
    // string literals are fine, but Rust constructs that materialize *absolute*
    // VA pointers — `match`-on-enum returning `&str`, static reference arrays,
    // trait-object vtables — load upper-half addresses that aren't mapped yet
    // and fault. Keep the pre-MMU path to literal logging + computed pointers
    // only. Once the MMU is on, both halves are mapped and such data resolves.
    klib::uart::init();
    klib::uart::Uart.println("Fermi OS (Rust) - Booting Up...");

    // Physical memory manager (pre-MMU, identity mapped).
    mm::pmm::init(mm::pmm::MEM_START, mm::pmm::MEM_SIZE);
    mm::pmm::print_info();

    // Build page tables and enable the MMU; keep the L1 handle for the tests.
    let l1_phys = mm::mmu::init();

    // MMU is on now — absolute-VA data resolves via TTBR1 even at physical PC.
    arch::cpu::print_current_el();

    // Install the exception vector table (VBAR_EL1).
    exception::init();

    // Self-tests run while TTBR0 is still the boot identity table.
    mm::mmu::run_tests(l1_phys);

    klib::uart::Uart.println("[BOOT] MMU Enabled. Jumping to Upper Half");
}

/// Upper-half kernel entry, branched to from `boot.S` after the MMU is enabled
/// and SP has been switched to the upper-half stack. Never returns.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    // Route all device MMIO through the TTBR1 upper half from here on.
    klib::mmio::switch_to_upper(mm::consts::KERNEL_VA_OFFSET as usize);

    // Re-point VBAR_EL1 at the upper-half vector table.
    exception::init_upper();

    // Relocate the PMM bitmap pointer to its upper-half virtual address.
    mm::pmm::relocate_upper();

    // Now in the upper half with RAM mapped Normal: core::fmt is safe.
    kprintln!("[KERNEL] kmain address: {:#x}", kmain as usize as u64);
    let sp: u64;
    unsafe { core::arch::asm!("mov {}, sp", out(reg) sp) };
    kprintln!("[KERNEL] Stack Pointer: {:#x}", sp);

    // CPU identification + PMU (ported later as a fuller module); for now the
    // heap is the next subsystem.
    mm::heap::init();
    mm::heap::run_tests();

    // Sanity-check the global allocator: alloc:: is now usable kernel-wide.
    {
        use alloc::vec::Vec;
        let mut v: Vec<u32> = Vec::new();
        for i in 0..8 {
            v.push(i * i);
        }
        kprintln!("[ALLOC] Vec demo: {:?}", v.as_slice());
    }

    // Exception self-test: a BRK is the one synchronous exception the dispatch
    // handles and resumes from (ELR += 4). Surviving it proves the vector table
    // is installed and the save/restore path round-trips correctly.
    kprintln!("[EXC TEST] Triggering BRK #0 ...");
    unsafe { core::arch::asm!("brk #0") };
    kprintln!("[EXC TEST] Survived BRK — vector table works.");

    // Interrupt controller. (Timer is started after the scheduler is up so the
    // first preemption has tasks to choose from.)
    exception::gic::init();

    // PCI bus enumeration + VirtIO device drivers.
    drivers::pci::enumerate_bus();
    drivers::virtio::rng::init();

    // RNG smoke test: pull a few random bytes.
    {
        let mut buf = [0u8; 16];
        let n = drivers::virtio::rng::read(&mut buf);
        kprintln!("[RNG TEST] got {} bytes: {:02x?}", n, &buf[..]);
    }

    drivers::virtio::blk::init();

    // BLK round-trip test: write a sector, read it back, compare.
    {
        use drivers::virtio::blk::VIRTIO_BLK_SECTOR_SIZE;
        let mut wbuf = [0u8; VIRTIO_BLK_SECTOR_SIZE];
        for (i, b) in wbuf.iter_mut().enumerate() {
            *b = (i & 0xFF) as u8;
        }
        let mut rbuf = [0u8; VIRTIO_BLK_SECTOR_SIZE];
        let wrote = drivers::virtio::blk::write(1, &wbuf);
        let read = drivers::virtio::blk::read(1, &mut rbuf);
        let ok = wrote && read && wbuf == rbuf;
        kprintln!("[BLK TEST] write+read sector 1 round-trip: {}", if ok { "PASS" } else { "FAIL" });
    }

    drivers::virtio::console::init();
    drivers::virtio::balloon::init();

    // Virtual filesystem + device nodes.
    fs::vfs::init();
    fs::devices::register();

    // VFS smoke test: open /dev/rng and read through the fd path.
    {
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/dev/rng");
        let mut buf = [0u8; 8];
        let n = fs::vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
        kprintln!("[VFS TEST] /dev/rng fd={} read {} bytes: {:02x?}", fd, n, &buf[..]);
        fs::vfs::fd_close(t, fd);
        fs::vfs::fd_table_destroy(t);
    }

    // Scheduler + a couple of demo kernel tasks (mirrors the original boot).
    sched::init();
    sched::create_task("task_a", task_a);
    sched::create_task("task_b", task_b);

    // Start the periodic timer: from here, timer IRQs drive preemption.
    exception::timer::init();
    exception::timer::start(exception::timer::TIMER_INTERVAL_MS);

    kprintln!("[KERNEL] Ready! Entering idle loop (preemptive scheduling active).");
    loop {
        sched::reap();
        unsafe { core::arch::asm!("wfi") };
    }
}

/// Demo task: prints a few iterations, yielding between each, then exits.
extern "C" fn task_a() {
    for i in 0..5 {
        kprintln!("[Task A] iteration {}", i);
        sched::r#yield();
    }
    kprintln!("[Task A] done! exiting");
}

/// Demo task: prints periodically forever (preempted by the timer).
extern "C" fn task_b() {
    loop {
        kprintln!("[Task B] running");
        for _ in 0..1_000_000 {
            core::hint::spin_loop();
        }
    }
}
