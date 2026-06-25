//! Interactive shell — reads lines from the console and dispatches builtins.
//!
//! Port of `task_shell` from the original `src/kernel.c`. Runs as an EL1 task
//! (reads via uart::getc, calls kernel services directly). Builtins map onto
//! the subsystems already ported: scheduler, /proc, VFS, net, balloon, etc.

use crate::exception::{gic, timer};
use crate::fs::vfs;
use crate::kprint;
use crate::kprintln;
use crate::mm::{heap, pmm};
use crate::net;
use crate::sched;
use crate::uart;
use crate::virtio;
use alloc::string::String;
use alloc::vec::Vec;
use crate::sync::Racy;

static HISTORY: Racy<Vec<String>> = Racy::new(Vec::new());
const HISTORY_MAX: usize = 16;

fn redraw(cur_len: usize, new: &[u8]) {
    for _ in 0..cur_len {
        uart::puts("\x08 \x08");
    }
    for &c in new {
        uart::putc(c);
    }
}

fn read_line(buf: &mut [u8]) -> usize {
    let hist = unsafe { HISTORY.get() };
    let mut hidx = hist.len();
    let mut len = 0;
    loop {
        let c = uart::getc();
        match c {
            b'\r' | b'\n' => {
                uart::putc(b'\n');
                return len;
            }
            0x7f | 0x08 => {
                if len > 0 {
                    len -= 1;
                    uart::puts("\x08 \x08");
                }
            }
            0x1b => {
                // ESC [ A/B — history up/down.
                if uart::getc() == b'[' {
                    let d = uart::getc();
                    let recalled: &[u8] = if d == b'A' {
                        if hidx > 0 { hidx -= 1; }
                        if hidx < hist.len() { hist[hidx].as_bytes() } else { b"" }
                    } else if d == b'B' {
                        if hidx < hist.len() { hidx += 1; }
                        if hidx < hist.len() { hist[hidx].as_bytes() } else { b"" }
                    } else {
                        b""
                    };
                    if d == b'A' || d == b'B' {
                        let n = core::cmp::min(recalled.len(), buf.len() - 1);
                        redraw(len, &recalled[..n]);
                        buf[..n].copy_from_slice(&recalled[..n]);
                        len = n;
                    }
                }
            }
            _ => {
                if len < buf.len() - 1 {
                    buf[len] = c;
                    len += 1;
                    uart::putc(c);
                }
            }
        }
    }
}

/// Append a command to the history ring.
fn history_push(line: &str) {
    if line.is_empty() {
        return;
    }
    let h = unsafe { HISTORY.get() };
    if h.last().map(|s| s.as_str()) == Some(line) {
        return; // skip consecutive duplicate
    }
    h.push(String::from(line));
    if h.len() > HISTORY_MAX {
        h.remove(0);
    }
}

fn parse_u64(s: &str) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u64;
    }
    Some(v)
}

fn cmd_cat(path: &str) {
    let node = vfs::resolve(path);
    if node.is_null() {
        kprintln!("cat: {}: not found", path);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, path);
    if fd >= 0 {
        let mut b = [0u8; 256];
        loop {
            let n = vfs::fd_read(t, fd, &mut b);
            if n <= 0 {
                break;
            }
            for &c in &b[..n as usize] {
                uart::putc(c);
            }
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
}

fn cmd_free() {
    let total = pmm::total_pages();
    let used = pmm::used_pages();
    kprintln!("Pages: total {} used {} free {}", total, used, total - used);
    kprintln!(
        "Heap:  used {} free {} total {} bytes",
        heap::used_bytes(),
        heap::free_bytes(),
        heap::total_bytes()
    );
}

fn cmd_run(path: &str) {
    let node = vfs::resolve(path);
    if node.is_null() {
        kprintln!("run: {}: not found", path);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, path);
    if fd >= 0 {
        let mut data: Vec<u8> = Vec::new();
        let mut chunk = [0u8; 512];
        loop {
            let n = vfs::fd_read(t, fd, &mut chunk);
            if n <= 0 { break; }
            data.extend_from_slice(&chunk[..n as usize]);
        }
        vfs::fd_close(t, fd);
        let pid = sched::spawn_elf("user", &data);
        kprintln!("run: spawned pid {}", pid);
    }
    vfs::fd_table_destroy(t);
}

fn dispatch(line: &str) {
    let mut parts = line.split_whitespace();
    let cmd = match parts.next() {
        Some(c) => c,
        None => return,
    };
    let arg1 = parts.next().unwrap_or("");
    match cmd {
        "help" => {
            kprintln!("builtins: help uptime version ps free meminfo ifconfig irqs");
            kprintln!("          cat <path> ping sleep <ms> kill <pid> echo <text>");
            kprintln!("          balloon <inflate|deflate|status> [n] vlog <text> clear");
            kprintln!("          ls [path] run <elf-path> cpuinfo reboot");
        }
        "uptime" => kprintln!("up {} ms ({} s)", timer::uptime_ms(), timer::uptime_seconds()),
        "history" => {
            let h = unsafe { HISTORY.get() };
            for (i, c) in h.iter().enumerate() {
                kprintln!("{:>3}  {}", i, c);
            }
        }
        "version" => kprintln!("Fermi OS (Rust) — aarch64, rustc 1.85.0"),
        "ps" => kprint!("{}", sched::render_tasks()),
        "free" | "meminfo" => cmd_free(),
        "ifconfig" => kprint!("{}", net::render_info()),
        "irqs" => kprint!("{}", gic::render_interrupts()),
        "ls" => {
            let path = if arg1.is_empty() { "/" } else { arg1 };
            kprint!("{}", vfs::list(path));
        }
        "cat" => {
            if arg1.is_empty() {
                kprintln!("usage: cat <path>");
            } else {
                cmd_cat(arg1);
            }
        }
        "ping" => {
            let ttl = net::ping(1);
            if ttl >= 0 {
                kprintln!("reply from gateway ttl={}", ttl);
            } else {
                kprintln!("ping: no reply");
            }
        }
        "sleep" => {
            if let Some(ms) = parse_u64(arg1) {
                sched::sleep_ms(ms);
            } else {
                kprintln!("usage: sleep <ms>");
            }
        }
        "kill" => {
            if let Some(pid) = parse_u64(arg1) {
                let r = sched::kill(pid);
                kprintln!("kill {}: {}", pid, if r == 0 { "ok" } else { "not found" });
            } else {
                kprintln!("usage: kill <pid>");
            }
        }
        "echo" => {
            let rest: Vec<&str> = line.splitn(2, ' ').collect();
            if rest.len() == 2 {
                kprintln!("{}", rest[1]);
            } else {
                kprintln!();
            }
        }
        "balloon" => {
            let n = parse_u64(parts.clone().next().unwrap_or("0")).unwrap_or(0) as u32;
            match arg1 {
                "inflate" => kprintln!("inflated {} pages", virtio::balloon::inflate(n)),
                "deflate" => kprintln!("deflated {} pages", virtio::balloon::deflate(n)),
                _ => {
                    let (a, t) = virtio::balloon::status();
                    kprintln!("balloon: actual {} pages, host_target {}", a, t);
                }
            }
        }
        "vlog" => {
            let rest: Vec<&str> = line.splitn(2, ' ').collect();
            if rest.len() == 2 {
                virtio::console::send(rest[1].as_bytes());
                virtio::console::send(b"\n");
                kprintln!("logged to /dev/vcons");
            }
        }
        "run" => {
            if arg1.is_empty() {
                kprintln!("usage: run <elf-path>");
            } else {
                cmd_run(arg1);
            }
        }
        "cpuinfo" => {
            let el = crate::cpu::current_el();
            kprintln!("EL{} | timer {} Hz", el, timer::get_frequency());
        }
        "clear" => kprint!("\x1b[2J\x1b[H"),
        "reboot" => {
            kprintln!("(reboot not wired; halting)");
            loop {
                unsafe { core::arch::asm!("wfi") };
            }
        }
        _ => kprintln!("unknown command: {} (try 'help')", cmd),
    }
}

/// Shell task entry (EL1).
pub extern "C" fn shell_task() {
    kprintln!("");
    kprintln!("Fermi OS shell. Type 'help' for builtins.");
    let mut line = [0u8; 256];
    loop {
        uart::puts("fermi> ");
        let n = read_line(&mut line);
        if n == 0 {
            continue;
        }
        let s = core::str::from_utf8(&line[..n]).unwrap_or("");
        let trimmed = s.trim();
        history_push(trimmed);
        dispatch(trimmed);
    }
}
