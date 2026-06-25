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

fn read_line(buf: &mut [u8]) -> usize {
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
                    uart::puts("\x08 \x08"); // erase
                }
            }
            _ => {
                if len < buf.len() - 1 {
                    buf[len] = c;
                    len += 1;
                    uart::putc(c); // echo
                }
            }
        }
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

fn cmd_run(path: &str, arg: &str) {
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
        let args: &[&str] = if arg.is_empty() { &[] } else { &[arg] };
        let pid = sched::spawn_elf("user", &data, args);
        kprintln!("run: spawned pid {}", pid);
    }
    vfs::fd_table_destroy(t);
}

fn cmd_write(name: &str, text: &str) {
    // Create a root-level FAT32 file from a string (newline-terminated).
    let mut data = alloc::vec::Vec::new();
    data.extend_from_slice(text.as_bytes());
    data.push(b'\n');
    let ok = crate::fs::fat32::create(name.as_bytes(), &data);
    kprintln!("write {}: {}", name, if ok { "ok" } else { "failed" });
}

fn cmd_hexdump(path: &str) {
    let node = vfs::resolve(path);
    if node.is_null() {
        kprintln!("hexdump: {}: not found", path);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, path);
    if fd >= 0 {
        let mut buf = [0u8; 16];
        let mut off = 0usize;
        loop {
            let n = vfs::fd_read(t, fd, &mut buf);
            if n <= 0 { break; }
            let n = n as usize;
            kprint!("{:08x}  ", off);
            for i in 0..16 {
                if i < n { kprint!("{:02x} ", buf[i]); } else { kprint!("   "); }
            }
            kprint!(" |");
            for i in 0..n {
                let c = buf[i];
                uart::putc(if (0x20..0x7f).contains(&c) { c } else { b'.' });
            }
            kprintln!("|");
            off += n;
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
}
fn cmd_top() {
    kprintln!("== Fermi OS top ==  uptime {} ms  cntpct {}",
              timer::uptime_ms(), timer::get_count());
    let total = pmm::total_pages();
    let used = pmm::used_pages();
    kprintln!("mem: {}/{} pages used  heap {} used / {} free bytes",
              used, total, heap::used_bytes(), heap::free_bytes());
    kprint!("{}", sched::render_tasks());
    kprint!("{}", crate::exception::gic::render_interrupts());
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
            kprintln!("builtins: help uptime version ps top free meminfo ifconfig irqs");
            kprintln!("          cat <path> ping sleep <ms> kill <pid> echo <text>");
            kprintln!("          balloon <inflate|deflate|status> [n] vlog <text> clear");
            kprintln!("          ls [path] write <name> <text> hexdump <path> run <elf> cpuinfo reboot");
        }
        "uptime" => kprintln!("up {} ms ({} s)", timer::uptime_ms(), timer::uptime_seconds()),
        "smp" => {
            if crate::smp::secondary_online() {
                let h1 = crate::smp::heartbeat();
                sched::sleep_ms(300);
                let h2 = crate::smp::heartbeat();
                kprintln!("core1 MPIDR={:#x} timer-ticks {} -> {} (preemptive)", crate::smp::secondary_mpidr(), h1, h2);
                let (a1, b1) = crate::smp::task_beats();
                sched::sleep_ms(300);
                let (a2, b2) = crate::smp::task_beats();
                kprintln!("  core1 task c1a: {} -> {} (+{})", a1, a2, a2 - a1);
                kprintln!("  core1 task c1b: {} -> {} (+{})", b1, b2, b2 - b1);
            } else {
                kprintln!("secondary not online (-smp 2)");
            }
        }
        "version" => kprintln!("Fermi OS (Rust) — aarch64, rustc 1.85.0"),
        "smptest" => {
            let (c0, c1, s0, s1, enq, rem) = crate::smp::wq_stats();
            let expect = if enq > 0 { enq * (enq - 1) / 2 } else { 0 };
            kprintln!("shared work queue: {} enqueued, {} remaining", enq, rem);
            kprintln!("  core0 processed: {} jobs (sum {})", c0, s0);
            kprintln!("  core1 processed: {} jobs (sum {})", c1, s1);
            kprintln!("  total {} jobs, checksum {} (expected {}) -> {}",
                      c0 + c1, s0 + s1, expect,
                      if c0 + c1 == enq && s0 + s1 == expect { "OK (no loss/dup)" } else { "MISMATCH" });
        }
        "ps" => kprint!("{}", sched::render_tasks()),
        "top" => cmd_top(),
        "free" | "meminfo" => cmd_free(),
        "ifconfig" => kprint!("{}", net::render_info()),
        "irqs" => kprint!("{}", gic::render_interrupts()),
        "write" => {
            let rest: Vec<&str> = line.splitn(3, ' ').collect();
            if rest.len() < 3 {
                kprintln!("usage: write <name.ext> <text>");
            } else {
                cmd_write(rest[1], rest[2]);
            }
        }
        "hexdump" => {
            if arg1.is_empty() { kprintln!("usage: hexdump <path>"); } else { cmd_hexdump(arg1); }
        }
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
        "http" => {
            if arg1.is_empty() { kprintln!("usage: http <host>"); }
            else if !net::http_get(arg1) { kprintln!("http: failed"); }
        }
        "mv" => {
            let rest: Vec<&str> = line.splitn(3, ' ').collect();
            if rest.len() < 3 {
                kprintln!("usage: mv <oldpath> <newpath>");
            } else if vfs::rename(rest[1], rest[2]) {
                kprintln!("renamed {} -> {}", rest[1], rest[2]);
            } else {
                kprintln!("mv: failed (missing source, dest exists, or not a FAT32 file)");
            }
        }
        "rm" => {
            if arg1.is_empty() {
                kprintln!("usage: rm <path>");
            } else if vfs::unlink(arg1) {
                kprintln!("removed {}", arg1);
            } else {
                kprintln!("rm: {}: not found or not a FAT32 file", arg1);
            }
        }
        "ping" => {
            let count = parse_u64(arg1).unwrap_or(4).max(1);
            let freq = timer::get_frequency().max(1);
            let mut received = 0u64;
            for seq in 1..=count {
                let t0 = timer::get_count();
                let ttl = net::ping(seq as u16);
                let t1 = timer::get_count();
                if ttl >= 0 {
                    received += 1;
                    let us = (t1.wrapping_sub(t0)) * 1_000_000 / freq;
                    kprintln!("reply seq={} ttl={} time={} us", seq, ttl, us);
                } else {
                    kprintln!("seq={} timeout", seq);
                }
                if seq < count {
                    sched::sleep_ms(200);
                }
            }
            let loss = (count - received) * 100 / count;
            kprintln!("--- {} sent, {} received, {}% loss", count, received, loss);
        }
        "ntp" => {
            let host = if arg1.is_empty() { "time.google.com" } else { arg1 };
            match net::ntp_query(host) {
                Some(t) => kprintln!("{}: network time = {} (unix epoch seconds)", host, t),
                None => kprintln!("ntp: no response from {}", host),
            }
        }
        "resolve" => {
            if arg1.is_empty() {
                kprintln!("usage: resolve <hostname>");
            } else {
                match net::resolve(arg1) {
                    Some(ip) => kprintln!("{} -> {}.{}.{}.{}", arg1, ip[0], ip[1], ip[2], ip[3]),
                    None => kprintln!("resolve: {}: no answer", arg1),
                }
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
        "rand" => {
            let n = parse_u64(arg1).unwrap_or(16).clamp(1, 64) as usize;
            let mut b = [0u8; 64];
            let got = virtio::rng::read(&mut b[..n]);
            kprint!("{} random bytes:", got);
            for &x in &b[..got] { kprint!(" {:02x}", x); }
            kprintln!();
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
                cmd_run(arg1, parts.next().unwrap_or(""));
            }
        }
        "cpuinfo" => {
            kprint!("{}", crate::cpu::render_info());
        }
        "clear" => kprint!("\x1b[2J\x1b[H"),
        "reboot" => {
            kprintln!("rebooting via PSCI...");
            crate::cpu::system_reset();
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
        let _ = String::from(s); // ensure UTF-8 path
        dispatch(s.trim());
    }
}
