//! `/proc` synthetic filesystem: each file regenerates its content on every
//! read from live kernel state. Pure Rust; generators render into a temp buffer
//! and `proc_read_via` serves the `[offset, offset+count)` slice with EOF.

use crate::arch::cpu;
use crate::drivers::virtio::balloon;
use crate::exception::{gic, timer};
use crate::fs::vfs::{self, File, FileOperations, Vnode, VnodeType};
use crate::klib::fmtbuf::FmtBuf;
use crate::kprintln;
use crate::mm::{heap, pmm};
use crate::sched;
use core::fmt::Write;

const PROC_BUF_BYTES: usize = 2048;

/// Run `gen` into a temp buffer, then serve [offset, offset+count) with EOF.
fn proc_read_via(gen: fn(&mut [u8]) -> usize, f: *mut File, buf: *mut u8, count: usize) -> i64 {
    let mut tmp = [0u8; PROC_BUF_BYTES];
    let avail = gen(&mut tmp);
    let offset = unsafe { (*f).offset as usize };
    if offset >= avail {
        return 0; // EOF
    }
    let remaining = avail - offset;
    let to_copy = core::cmp::min(count, remaining);
    unsafe {
        core::ptr::copy_nonoverlapping(tmp.as_ptr().add(offset), buf, to_copy);
        (*f).offset += to_copy as i64;
    }
    to_copy as i64
}

// --- Generators -------------------------------------------------------------

fn gen_uptime(out: &mut [u8]) -> usize {
    let ms = timer::uptime_ms();
    let s = ms / 1000;
    let cs = (ms % 1000) / 10;
    let mut w = FmtBuf::new(out);
    let _ = write!(w, "{}.{}{}\n", s, cs / 10, cs % 10);
    w.len()
}

fn gen_meminfo(out: &mut [u8]) -> usize {
    let mut w = FmtBuf::new(out);
    let _ = write!(
        w,
        "MemTotal:    {} KB\n\
         MemUsed:     {} KB\n\
         MemFree:     {} KB\n\
         MemReserved: {} KB\n\
         HeapTotal:   {} KB\n\
         HeapUsed:    {} KB\n\
         HeapFree:    {} KB\n",
        pmm::total_pages() * 4,
        pmm::used_pages() * 4,
        pmm::free_pages_count() * 4,
        pmm::reserved_pages() * 4,
        heap::total_bytes() / 1024,
        heap::used_bytes() / 1024,
        heap::free_bytes() / 1024,
    );
    w.len()
}

fn gen_tasks(out: &mut [u8]) -> usize {
    let mut w = FmtBuf::new(out);
    let _ = write!(w, "PID  STATE     NAME\n---- --------- ----------------\n");
    // SAFETY (single-core): run-queue walk; tasks aren't freed concurrently here.
    unsafe {
        let head = sched::first_task();
        let mut t = head;
        loop {
            let _ = write!(
                w,
                "{}  {}   {}\n",
                (*t).pid,
                sched::state_name((*t).state),
                sched::task_name(t)
            );
            t = (*t).next;
            if t.is_null() || t == head {
                break;
            }
        }
    }
    w.len()
}

fn gen_netinfo(out: &mut [u8]) -> usize {
    crate::drivers::virtio::net::get_info(out)
}

fn gen_interrupts(out: &mut [u8]) -> usize {
    gic::render_interrupts(out)
}

fn gen_cmdline(out: &mut [u8]) -> usize {
    let mut w = FmtBuf::new(out);
    let _ = write!(
        w,
        "console=ttyAMA0 maxcpus=1 net=virtio-net-pci ip=10.0.2.15\n"
    );
    w.len()
}

fn gen_version(out: &mut [u8]) -> usize {
    let mut w = FmtBuf::new(out);
    let _ = write!(w, "Fermi OS aarch64 (cortex-a72)\nRust port\n");
    w.len()
}

fn gen_balloon(out: &mut [u8]) -> usize {
    let (actual, target) = balloon::status();
    let mut w = FmtBuf::new(out);
    let _ = write!(
        w,
        "actual:      {} pages ({} KB)\nhost_target: {} pages ({} KB)\n",
        actual,
        actual * 4,
        target,
        target * 4
    );
    w.len()
}

fn gen_cpuinfo(out: &mut [u8]) -> usize {
    cpu::render_info(out)
}

// --- file_operations wrappers (one read fn per generator) -------------------

macro_rules! proc_file {
    ($read_fn:ident, $gen:ident, $ops:ident) => {
        fn $read_fn(_n: *mut Vnode, f: *mut File, buf: *mut u8, count: usize) -> i64 {
            proc_read_via($gen, f, buf, count)
        }
        static $ops: FileOperations = FileOperations {
            read: Some($read_fn),
            write: None,
        };
    };
}

proc_file!(read_uptime, gen_uptime, UPTIME_OPS);
proc_file!(read_meminfo, gen_meminfo, MEMINFO_OPS);
proc_file!(read_tasks, gen_tasks, TASKS_OPS);
proc_file!(read_netinfo, gen_netinfo, NETINFO_OPS);
proc_file!(read_interrupts, gen_interrupts, INTERRUPTS_OPS);
proc_file!(read_cmdline, gen_cmdline, CMDLINE_OPS);
proc_file!(read_balloon, gen_balloon, BALLOON_OPS);
proc_file!(read_cpuinfo, gen_cpuinfo, CPUINFO_OPS);
proc_file!(read_version, gen_version, VERSION_OPS);

fn register_file(parent: *mut Vnode, name: &str, ops: *const FileOperations) {
    let n = vfs::create_node(parent, name, VnodeType::Reg);
    if n.is_null() {
        kprintln!("[PROC] Failed to create /proc/{}", name);
        return;
    }
    unsafe { (*n).ops = ops };
}

/// Mount `/proc` and register all synthetic files.
pub fn init() {
    kprintln!("[PROC] Initializing /proc");
    let proc = vfs::create_node(vfs::root(), "proc", VnodeType::Dir);
    if proc.is_null() {
        crate::klib::uart::Uart.errorln("[PROC] Failed to create /proc");
        return;
    }

    register_file(proc, "uptime", &UPTIME_OPS);
    register_file(proc, "meminfo", &MEMINFO_OPS);
    register_file(proc, "tasks", &TASKS_OPS);
    register_file(proc, "netinfo", &NETINFO_OPS);
    register_file(proc, "interrupts", &INTERRUPTS_OPS);
    register_file(proc, "cmdline", &CMDLINE_OPS);
    register_file(proc, "balloon", &BALLOON_OPS);
    register_file(proc, "cpuinfo", &CPUINFO_OPS);
    register_file(proc, "version", &VERSION_OPS);

    kprintln!("[PROC] Mounted at /proc (uptime, meminfo, tasks, interrupts, netinfo, cmdline, version, balloon, cpuinfo)");
}
