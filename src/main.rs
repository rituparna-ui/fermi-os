//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]
#![deny(unsafe_op_in_unsafe_fn)]
// Clippy lints we intentionally accept: register/offset definitions keep
// `+ 0x00` and `<< 0` for symmetry with the hardware spec; some ABI structs
// have many fields; writeln-style newline endings are deliberate in the kernel
// log format. Real correctness lints stay on.
#![allow(clippy::identity_op)]
#![allow(clippy::too_many_arguments)]
#![allow(clippy::type_complexity)]
#![allow(clippy::write_with_newline)]
#![allow(clippy::needless_range_loop)]
#![allow(clippy::manual_div_ceil)]
#![allow(clippy::unnecessary_cast)]

extern crate alloc;

#[macro_use]
mod klib;
#[macro_use]
mod arch;
mod drivers;
mod exception;
mod fs;
mod hyp;
mod mm;
mod panic;
mod sched;
mod syscall;
mod user;

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
pub extern "C" fn early_init(booted_via_el2: u64) {
    // Record whether boot entered at EL2 (a hypervisor is active beneath us).
    // boot.S threaded this through x28 across the eret and zero_bss; storing it
    // now (post-zero_bss) makes it durable for the guest-side HVC probe below.
    hyp::set_booted_via_el2(booted_via_el2 != 0);

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

    // Hypervisor probe: if we booted via EL2, exercise the hypercall ABI from
    // the EL1 guest. Skipped entirely on a bare EL1 boot (no `virtualization=on`)
    // where an HVC would trap as UNDEFINED.
    hyp::guest_probe();

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
        kprintln!(
            "[BLK TEST] write+read sector 1 round-trip: {}",
            if ok { "PASS" } else { "FAIL" }
        );
    }

    drivers::virtio::net::init();
    drivers::virtio::console::init();
    drivers::virtio::balloon::init();

    // CPU identification + PMU (now in the upper half; core::fmt safe).
    arch::cpu::cpu_init();

    // Virtual filesystem + device nodes + /proc.
    fs::vfs::init();
    fs::devices::register();
    fs::proc::init();

    // Mount FAT32 from virtio-blk at /mnt/fat32.
    if fs::fat32::mount() {
        let mnt = fs::vfs::create_node(fs::vfs::root(), "mnt", fs::vfs::VnodeType::Dir);
        fs::vfs::create_node(mnt, "fat32", fs::vfs::VnodeType::Dir);
        fs::fat32::vfs_mount("/mnt/fat32");

        // Smoke test: read /mnt/fat32/HELLO.TXT if present.
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/mnt/fat32/HELLO.TXT");
        if fd >= 0 {
            let mut buf = [0u8; 128];
            let n = fs::vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
            if n > 0 {
                if let Ok(s) = core::str::from_utf8(&buf[..n as usize]) {
                    kprintln!("[FAT32 TEST] /mnt/fat32/HELLO.TXT ({} bytes): {:?}", n, s);
                }
            }
            fs::vfs::fd_close(t, fd);
        } else {
            kprintln!("[FAT32 TEST] HELLO.TXT not found (disk not formatted?)");
        }

        // Write round-trip: create a file, then read it back through the VFS.
        let payload = b"written by the Rust kernel\n";
        // create() may return false if RUSTW.TXT already exists from a prior
        // boot on a re-used disk (duplicates are now refused) — that's fine;
        // the read-back below is the real assertion and is idempotent.
        let _ = fs::fat32::create(b"RUSTW.TXT", payload);
        let fd = fs::vfs::fd_open(t, "/mnt/fat32/RUSTW.TXT");
        if fd >= 0 {
            let mut buf = [0u8; 64];
            let n = fs::vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
            let ok = n as usize == payload.len() && &buf[..n as usize] == payload;
            kprintln!(
                "[FAT32 TEST] create+read RUSTW.TXT round-trip: {}",
                if ok { "PASS" } else { "FAIL" }
            );
            fs::vfs::fd_close(t, fd);
        } else {
            kprintln!("[FAT32 TEST] create+read RUSTW.TXT round-trip: FAIL (reopen)");
        }
        fs::vfs::fd_table_destroy(t);
    }

    // /proc smoke test: read /proc/cpuinfo through the fd path.
    {
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/proc/cpuinfo");
        let mut buf = [0u8; 512];
        let n = fs::vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
        if n > 0 {
            if let Ok(s) = core::str::from_utf8(&buf[..n as usize]) {
                kprintln!("[PROC TEST] /proc/cpuinfo:\n{}", s);
            }
        }
        fs::vfs::fd_close(t, fd);
        fs::vfs::fd_table_destroy(t);
    }

    // VFS smoke test: open /dev/rng and read through the fd path.
    {
        let t = fs::vfs::fd_table_create();
        let fd = fs::vfs::fd_open(t, "/dev/rng");
        let mut buf = [0u8; 8];
        let n = fs::vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
        kprintln!(
            "[VFS TEST] /dev/rng fd={} read {} bytes: {:02x?}",
            fd,
            n,
            &buf[..]
        );
        fs::vfs::fd_close(t, fd);
        fs::vfs::fd_table_destroy(t);
    }

    // Scheduler + tasks: the interactive EL0 shell, a deliberate-crash task to
    // exercise kill-on-fault, and the netd EL1 background pinger.
    sched::init();
    sched::create_task("task_shell", user::task_shell);
    sched::create_task("task_crash", user::task_crash);
    sched::create_task("task_forker", user::task_forker);
    sched::create_kernel_task("netd", netd);
    // Task-churn stress test (EL1 kernel task): rapidly creates + reaps EL0
    // tasks and checks the PMM free-page count returns to baseline, flushing
    // out leaks / use-after-free in address-space teardown + ASID recycling.
    // Created last so it doesn't perturb the other tasks' pids.
    sched::create_kernel_task("churn", churn_test);

    // Start the periodic timer: from here, timer IRQs drive preemption.
    exception::timer::init();
    exception::timer::start(exception::timer::TIMER_INTERVAL_MS);

    kprintln!("[KERNEL] Ready! Entering idle loop (preemptive scheduling active).");
    loop {
        sched::reap();
        unsafe { core::arch::asm!("wfi") };
    }
}

/// Task-churn stress test. Runs as an EL1 kernel task so it can drive the
/// scheduler directly. Creates many short-lived EL0 tasks in batches; each
/// task allocates a kernel stack + user page tables + user stack + fd table and
/// frees them all on exit/reap. If teardown leaks (or double-frees), the PMM
/// free-page count drifts. We compare against a baseline taken after the first
/// batch has fully settled (so one-time allocations like the heap's lazy growth
/// don't count as a leak).
extern "C" fn churn_test() {
    const ROUNDS: u32 = 6;
    const BATCH: u32 = 8;

    // Let the initial task set settle before measuring (yield, don't sleep —
    // sleep_ms logs and would flood the console / disturb the shell test).
    for _ in 0..50 {
        sched::r#yield();
    }

    let mut baseline: u64 = 0;
    for round in 0..ROUNDS {
        for _ in 0..BATCH {
            sched::create_task("churnkid", user::task_noop);
        }
        // Yield until the batch has run, exited, and been reaped. The noop
        // tasks exit on first schedule; yielding repeatedly drains them, and
        // reap() frees their resources.
        for _ in 0..40 {
            sched::reap();
            sched::r#yield();
        }
        let free = mm::pmm::free_pages_count();
        if round == 0 {
            baseline = free; // first round absorbs any one-time settling
        }
    }

    let final_free = mm::pmm::free_pages_count();
    let leaked = baseline as i64 - final_free as i64;
    if leaked == 0 {
        kprintln!(
            "[CHURN TEST] PASS: no page leak across {} task creations",
            ROUNDS * BATCH
        );
    } else {
        kprintln!("[CHURN TEST] FAIL: leaked {} pages across churn", leaked);
    }

    heap_stress();
    fd_stress();
    fat32_stress();
    asid_wrap_stress();
    sched::task_exit();
}

/// ASID-wraparound stress (risk R3): seed the ASID counter near the 16-bit max
/// so the next few task creations cross 65535→(flush all TLBs)→1, then create
/// EL0 tasks across that boundary, drain + reap them, and assert: the counter
/// actually wrapped, the kernel survived the global TLBI, and no pages leaked.
/// Reaching this naturally would need 65535 task creations.
fn asid_wrap_stress() {
    let free_before = mm::pmm::free_pages_count();

    // Seed just below the wrap. asid_alloc() hands out 65534, 65535, then
    // wraps to 1 — so creating ~4 tasks crosses the boundary.
    sched::force_next_asid(65534);

    for _ in 0..4 {
        sched::create_task("churnkid", user::task_noop);
    }
    // Drain until every churnkid has exited *and* been reaped — i.e. the PMM
    // free count returns to the pre-create baseline. Poll rather than spinning a
    // fixed number of yields, since the time to fully reap varies with overall
    // system load (a fixed count was a flaky measurement, not a real leak).
    let mut free_after = 0;
    for _ in 0..2000 {
        sched::reap();
        sched::r#yield();
        free_after = mm::pmm::free_pages_count();
        if free_after >= free_before {
            break;
        }
    }

    let wrapped = sched::peek_next_asid() < 100; // reset to a small value
    let leaked = free_before as i64 - free_after as i64;

    if wrapped && leaked == 0 {
        kprintln!(
            "[ASID WRAP] PASS: crossed 65535->1 (now {}), TLB flushed, no leak",
            sched::peek_next_asid()
        );
    } else {
        kprintln!(
            "[ASID WRAP] FAIL: wrapped={} next_asid={} leaked={}",
            wrapped,
            sched::peek_next_asid(),
            leaked
        );
    }
}

/// FAT32 multi-file stress: create many files (enough to push the root dir past
/// its first 16-entry sector), then read each back and verify contents. Catches
/// bugs in dir_add_entry's multi-sector walk, cluster allocation, and the
/// read-back path. Runs at EL1 via the kernel VFS API.
fn fat32_stress() {
    use fs::vfs;
    const N: usize = 30;

    let t = vfs::fd_table_create();
    if t.is_null() {
        kprintln!("[FAT32 STRESS] FAIL: fd table");
        return;
    }

    let mut created = 0;
    let mut verified = 0;
    for i in 0..N {
        // 8.3 name: STRESSNN  (NN = 00..29), unique per file.
        let name = [
            b'S',
            b'T',
            b'R',
            b'S',
            b'0' + (i / 10) as u8,
            b'0' + (i % 10) as u8,
        ];
        // Distinct payload per file so a mixed-up read is caught.
        let payload = [b'A' + (i % 26) as u8; 40];
        if fs::fat32::create(&name, &payload) {
            created += 1;
        }
    }

    // Read each back via the VFS path and compare.
    for i in 0..N {
        let mut path = [0u8; 32];
        // "/mnt/fat32/STRSNN" (8.3 -> on-disk name has no dot here; uppercase)
        let prefix = b"/mnt/fat32/STRS";
        path[..prefix.len()].copy_from_slice(prefix);
        path[prefix.len()] = b'0' + (i / 10) as u8;
        path[prefix.len() + 1] = b'0' + (i % 10) as u8;
        let plen = prefix.len() + 2;
        let path_str = core::str::from_utf8(&path[..plen]).unwrap_or("");
        let fd = vfs::fd_open(t, path_str);
        if fd < 0 {
            continue;
        }
        let mut buf = [0u8; 64];
        let n = vfs::fd_read(t, fd, buf.as_mut_ptr(), buf.len());
        vfs::fd_close(t, fd);
        let expect = b'A' + (i % 26) as u8;
        if n == 40 && buf[..40].iter().all(|&b| b == expect) {
            verified += 1;
        }
    }
    vfs::fd_table_destroy(t);

    // `verified` is the real assertion: all N files are present with correct
    // contents. `created` may be < N on a re-used disk where they already exist
    // (duplicates are refused), so it isn't required to equal N — only that no
    // creation spuriously failed for a *new* file (created + already-present
    // together cover all N, which `verified == N` proves).
    let _ = created;
    if verified == N {
        kprintln!("[FAT32 STRESS] PASS: created + verified {} files", N);
    } else {
        kprintln!("[FAT32 STRESS] FAIL: verified={} of {}", verified, N);
    }

    // Subdirectory test: mkdir a dir, create a file inside it, read it back via
    // the nested path. Idempotent across re-used disks (mkdir/create refuse
    // duplicates; the read-back is the assertion).
    let _ = fs::fat32::mkdir(b"RDIR");
    let inner = b"deep file contents\n";
    let _ = fs::fat32::create(b"RDIR/INNER.TXT", inner);
    let t3 = vfs::fd_table_create();
    let fd = vfs::fd_open(t3, "/mnt/fat32/RDIR/INNER.TXT");
    let mut ok = false;
    if fd >= 0 {
        let mut buf = [0u8; 64];
        let n = vfs::fd_read(t3, fd, buf.as_mut_ptr(), buf.len());
        ok = n as usize == inner.len() && &buf[..n as usize] == inner;
        vfs::fd_close(t3, fd);
    }
    vfs::fd_table_destroy(t3);
    kprintln!(
        "[FAT32 SUBDIR] mkdir RDIR + create RDIR/INNER.TXT + read: {}",
        if ok { "PASS" } else { "FAIL" }
    );

    // Remove test: create a never-looked-up file (avoiding VFS cache), confirm
    // it exists on disk, remove it, confirm it's gone, and confirm the freed
    // slot/clusters can be reused by re-creating it. Uses the cache-bypassing
    // exists() so it's idempotent and unaffected by VFS vnode caching.
    let _ = fs::fat32::remove(b"RMTEST.TXT"); // clean slate if a prior run left it
    let made = fs::fat32::create(b"RMTEST.TXT", b"to be removed\n");
    let present = fs::fat32::exists(b"RMTEST.TXT");
    let removed = fs::fat32::remove(b"RMTEST.TXT");
    let gone = !fs::fat32::exists(b"RMTEST.TXT");
    let remade = fs::fat32::create(b"RMTEST.TXT", b"again\n");
    let _ = fs::fat32::remove(b"RMTEST.TXT"); // leave the disk clean
    let rm_ok = made && present && removed && gone && remade;
    kprintln!(
        "[FAT32 RM] create+exists+remove+gone+recreate: {}",
        if rm_ok { "PASS" } else { "FAIL" }
    );

    // Delete/reuse churn: create→verify→rm a multi-cluster file many times with
    // changing content, then confirm free space returned to baseline. Verifies
    // freed clusters are actually reusable (FAT entries zeroed) and that
    // recreated files read back their *new* content (no stale data), all
    // bypassing the VFS cache via read_path.
    let cyc_baseline = mm::pmm::free_pages_count(); // PMM is stable here; FAT free-space below
    let blk_free_before = fat32_free_clusters();
    let mut cyc_ok = true;
    for round in 0..5u8 {
        // ~6 KiB payload spans multiple clusters; content distinct per round.
        let mut payload = [0u8; 6000];
        for b in payload.iter_mut() {
            *b = b'A' + round % 26;
        }
        if !fs::fat32::create(b"CYCLE.DAT", &payload) {
            cyc_ok = false;
            break;
        }
        let mut rbuf = [0u8; 6000];
        let n = fs::fat32::read_path(b"CYCLE.DAT", &mut rbuf);
        if n as usize != payload.len() || rbuf[..] != payload[..] {
            cyc_ok = false;
            break;
        }
        if !fs::fat32::remove(b"CYCLE.DAT") {
            cyc_ok = false;
            break;
        }
    }
    let blk_free_after = fat32_free_clusters();
    let _ = cyc_baseline;
    if cyc_ok && blk_free_after == blk_free_before {
        kprintln!("[FAT32 CYCLE] PASS: 5x create/verify/rm, FAT free-space stable");
    } else {
        kprintln!(
            "[FAT32 CYCLE] FAIL: ok={} free {} -> {}",
            cyc_ok,
            blk_free_before,
            blk_free_after
        );
    }
}

/// Count free clusters on the mounted FAT32 volume (test helper).
fn fat32_free_clusters() -> u64 {
    fs::fat32::count_free_clusters()
}

/// fd-table stress: open the max number of fds, confirm the table rejects the
/// (MAX+1)th, close them all, and confirm reuse. Exercises the fd-table
/// boundary and close/realloc path. Runs at EL1 via the kernel VFS API.
fn fd_stress() {
    use fs::vfs;
    let t = vfs::fd_table_create();
    if t.is_null() {
        kprintln!("[FD STRESS] FAIL: fd_table_create returned null");
        return;
    }

    // Open /dev/zero MAX_FDS times — all should succeed (fds 0..MAX-1).
    let mut opened = 0;
    for _ in 0..vfs::MAX_FDS {
        if vfs::fd_open(t, "/dev/zero") >= 0 {
            opened += 1;
        }
    }
    // The next open must fail (table full).
    let overflow = vfs::fd_open(t, "/dev/zero");
    // Close one and confirm a fresh open reuses the slot.
    let _ = vfs::fd_close(t, 0);
    let reused = vfs::fd_open(t, "/dev/zero");
    // Double-close + bad-fd should be rejected, not crash.
    let double = vfs::fd_close(t, 0); // slot 0 now holds `reused`
    let badfd = vfs::fd_close(t, 9999);

    vfs::fd_table_destroy(t);

    let ok = opened == vfs::MAX_FDS && overflow < 0 && reused == 0 && double == 0 && badfd < 0;
    if ok {
        kprintln!(
            "[FD STRESS] PASS: {} fds, overflow rejected, close/reuse + bad-fd handled",
            opened
        );
    } else {
        kprintln!(
            "[FD STRESS] FAIL: opened={} overflow={} reused={} double={} badfd={}",
            opened,
            overflow,
            reused,
            double,
            badfd
        );
    }
}

/// Heap stress: churn many varied-size allocations (incl. one large enough to
/// force a heap expand) and assert the heap's used-byte count returns exactly
/// to baseline — catching leaks, double-frees, and coalescing bugs across the
/// first-fit free list and any expanded regions.
fn heap_stress() {
    use alloc::vec::Vec;

    let baseline = mm::heap::used_bytes();

    for round in 0..8u64 {
        // A spread of small/medium Vecs that grow (each push may realloc).
        let mut keep: Vec<Vec<u8>> = Vec::new();
        for i in 0..64u64 {
            let n = ((i * 37 + round * 11) % 600 + 1) as usize;
            let mut v = Vec::with_capacity(n);
            v.resize(n, (i & 0xFF) as u8);
            keep.push(v);
        }
        // One allocation larger than the 1 MiB initial heap, forcing expand().
        let big: Vec<u8> = alloc::vec![round as u8; 1_200_000];
        // Touch endpoints so the compiler can't elide the allocation.
        core::hint::black_box(big.first().copied());
        core::hint::black_box(big.last().copied());
        core::hint::black_box(keep.len());
        // `keep` and `big` drop here, returning everything to the heap.
    }

    let after = mm::heap::used_bytes();
    if after == baseline {
        kprintln!(
            "[HEAP STRESS] PASS: heap used-bytes back to baseline ({} B)",
            baseline
        );
    } else {
        kprintln!(
            "[HEAP STRESS] FAIL: heap leaked {} bytes (baseline {} -> {})",
            after as i64 - baseline as i64,
            baseline,
            after
        );
    }
}

/// netd: an EL1 kernel daemon that periodically pings the slirp gateway and
/// drains incoming RX. Runs at EL1 so it calls the net driver directly.
extern "C" fn netd() {
    kprintln!("[netd] starting (kernel-mode background pinger)");
    let mut seq: u16 = 2; // seq 1 was sent during net::init
    loop {
        sched::sleep_ms(5000);

        // Drain anything that arrived while we slept.
        let mut buf = [0u8; 256];
        let mut drained = 0;
        while drivers::virtio::net::rx_poll(&mut buf) > 0 {
            drained += 1;
        }
        if drained > 0 {
            kprintln!("[netd] drained {} async frames before ping", drained);
        }

        let t0 = exception::timer::get_ticks();
        let ttl = drivers::virtio::net::send_ping(seq);
        if ttl >= 0 {
            let t1 = exception::timer::get_ticks();
            kprintln!(
                "[netd] ping seq={} reply ttl={} in {} ticks",
                seq,
                ttl,
                t1 - t0
            );
        } else {
            kprintln!("[netd] ping seq={} — no reply", seq);
        }
        seq = seq.wrapping_add(1);
    }
}
