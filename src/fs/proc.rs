//! /proc synthetic filesystem — content regenerated per read from live state.
//!
//! Port of `src/fs/proc/*`. Files are VFS regular nodes whose DevOps is
//! `Proc(kind)`; reading one calls [`generate`].

use super::vfs::{self, ProcKind, VnodeType};
use crate::exception::{gic, timer};
use crate::kprintln;
use crate::mm::{heap, pmm};
use crate::net;
use crate::sched;
use alloc::string::String;
use core::fmt::Write;

pub fn generate(kind: ProcKind) -> String {
    let mut s = String::new();
    match kind {
        ProcKind::Uptime => {
            let _ = writeln!(s, "{} ms ({} s)", timer::uptime_ms(), timer::uptime_seconds());
        }
        ProcKind::Meminfo => {
            let total = pmm::total_pages();
            let used = pmm::used_pages();
            let _ = writeln!(s, "MemTotalPages: {}", total);
            let _ = writeln!(s, "MemUsedPages:  {}", used);
            let _ = writeln!(s, "MemFreePages:  {}", total - used);
            let _ = writeln!(s, "MemReserved:   {}", pmm::reserved_pages());
            let _ = writeln!(s, "HeapUsed:      {} bytes", heap::used_bytes());
            let _ = writeln!(s, "HeapFree:      {} bytes", heap::free_bytes());
            let _ = writeln!(s, "HeapTotal:     {} bytes", heap::total_bytes());
        }
        ProcKind::Tasks => {
            s = sched::render_tasks();
        }
        ProcKind::Interrupts => {
            s = gic::render_interrupts();
        }
        ProcKind::Netinfo => {
            s = net::render_info();
        }
        ProcKind::Cmdline => {
            let _ = writeln!(s, "fermi-os console=ttyAMA0 root=/dev/blk");
        }
        ProcKind::Version => {
            let _ = writeln!(s, "Fermi OS (Rust) — bare-metal aarch64 kernel");
            let _ = writeln!(s, "rustc {}, target aarch64-unknown-none", "1.85.0");
        }
        ProcKind::Cpuinfo => {
            s = crate::cpu::render_info();
        }
    }
    s
}

/// Create /proc and its synthetic files.
pub fn mount() {
    let root = vfs::resolve("/");
    let proc = vfs::create_node(root, "proc", VnodeType::Dir);
    let files = [
        ("uptime", ProcKind::Uptime),
        ("meminfo", ProcKind::Meminfo),
        ("tasks", ProcKind::Tasks),
        ("interrupts", ProcKind::Interrupts),
        ("netinfo", ProcKind::Netinfo),
        ("cmdline", ProcKind::Cmdline),
        ("version", ProcKind::Version),
        ("cpuinfo", ProcKind::Cpuinfo),
    ];
    for (name, kind) in files {
        let n = vfs::create_node(proc, name, VnodeType::Reg);
        if !n.is_null() {
            unsafe {
                (*n).dev = vfs::DevOps::Proc(kind);
            }
        }
    }
    kprintln!("[PROC] Mounted /proc with {} files", files.len());
}
