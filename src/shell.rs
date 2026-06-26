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

const BUILTINS: &[&str] = &[
    "help", "uptime", "version", "ps", "free", "meminfo", "ifconfig", "irqs",
    "cat", "ping", "sleep", "kill", "echo", "balloon", "vlog", "cpuinfo",
    "clear", "reboot", "ls", "run",
];

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
            0x09 => {
                // Tab: complete the (single-word) command against BUILTINS.
                let prefix = core::str::from_utf8(&buf[..len]).unwrap_or("");
                if !prefix.is_empty() && !prefix.contains(' ') {
                    let mut only: Option<&str> = None;
                    let mut count = 0;
                    for b in BUILTINS {
                        if b.starts_with(prefix) {
                            count += 1;
                            only = Some(b);
                        }
                    }
                    if count == 1 {
                        let comp = only.unwrap();
                        let rest = &comp.as_bytes()[prefix.len()..];
                        let n = core::cmp::min(rest.len(), buf.len() - 1 - len);
                        for &c in &rest[..n] { uart::putc(c); buf[len] = c; len += 1; }
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

fn line_has(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}

fn cmd_grep(pattern: &str, path: &str) {
    if vfs::resolve(path).is_null() {
        kprintln!("grep: {}: not found", path);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, path);
    let needle = pattern.as_bytes();
    let mut matches = 0u32;
    if fd >= 0 {
        let mut b = [0u8; 256];
        let mut line = [0u8; 1024];
        let mut ll = 0usize;
        let emit = |line: &[u8]| {
            if line_has(line, needle) {
                for &c in line {
                    uart::putc(c);
                }
                uart::putc(b'\n');
                return true;
            }
            false
        };
        loop {
            let n = vfs::fd_read(t, fd, &mut b);
            if n <= 0 {
                break;
            }
            for &c in &b[..n as usize] {
                if c == b'\n' {
                    if emit(&line[..ll]) {
                        matches += 1;
                    }
                    ll = 0;
                } else if ll < line.len() {
                    line[ll] = c;
                    ll += 1;
                }
            }
        }
        if ll > 0 && emit(&line[..ll]) {
            matches += 1;
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
    kprintln!("grep: {} matching line(s)", matches);
}

fn cmd_append(path: &str, text: &str) {
    use alloc::vec::Vec;
    let mut data: Vec<u8> = Vec::new();
    let existed = !vfs::resolve(path).is_null();
    if existed {
        let t = vfs::fd_table_create();
        let fd = vfs::fd_open(t, path);
        if fd >= 0 {
            let mut b = [0u8; 256];
            loop {
                let n = vfs::fd_read(t, fd, &mut b);
                if n <= 0 { break; }
                data.extend_from_slice(&b[..n as usize]);
            }
            vfs::fd_close(t, fd);
        }
        vfs::fd_table_destroy(t);
    }
    data.extend_from_slice(text.as_bytes());
    data.push(b'\n');
    let name = path.rsplit('/').next().unwrap_or("");
    if existed { vfs::unlink(path); }
    let ok = crate::fs::fat32::create(name.as_bytes(), &data);
    kprintln!("append {}: {} ({} bytes)", path, if ok { "ok" } else { "failed" }, data.len());
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

fn cmd_wc(path: &str) {
    let node = vfs::resolve(path);
    if node.is_null() {
        kprintln!("wc: {}: not found", path);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, path);
    let (mut lines, mut words, mut bytes) = (0u64, 0u64, 0u64);
    let mut in_word = false;
    if fd >= 0 {
        let mut buf = [0u8; 256];
        loop {
            let n = vfs::fd_read(t, fd, &mut buf);
            if n <= 0 { break; }
            for &c in &buf[..n as usize] {
                bytes += 1;
                if c == b'\n' { lines += 1; }
                if c == b' ' || c == b'\n' || c == b'\t' || c == b'\r' {
                    in_word = false;
                } else if !in_word {
                    in_word = true;
                    words += 1;
                }
            }
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
    kprintln!("{:>6} {:>6} {:>6} {}", lines, words, bytes, path);
}

fn cmd_cp(src: &str, dst_name: &str) {
    let node = vfs::resolve(src);
    if node.is_null() {
        kprintln!("cp: {}: not found", src);
        return;
    }
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, src);
    let mut data = alloc::vec::Vec::new();
    if fd >= 0 {
        let mut chunk = [0u8; 512];
        loop {
            let n = vfs::fd_read(t, fd, &mut chunk);
            if n <= 0 { break; }
            data.extend_from_slice(&chunk[..n as usize]);
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
    let ok = crate::fs::fat32::create(dst_name.as_bytes(), &data);
    kprintln!("cp {} -> {} ({} bytes): {}", src, dst_name, data.len(),
              if ok { "ok" } else { "failed" });
}

fn cmd_blkdump(sector: u64) {
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, "/dev/blk");
    if fd >= 0 {
        vfs::fd_seek(t, fd, (sector * 512) as i64, vfs::SEEK_SET);
        let mut b = alloc::vec![0u8; 512];
        if vfs::fd_read(t, fd, &mut b) == 512 {
            for row in 0..4 {
                kprint!("{:08x}  ", sector * 512 + row * 16);
                for i in 0..16 { kprint!("{:02x} ", b[row as usize * 16 + i]); }
                kprint!(" |");
                for i in 0..16 {
                    let c = b[row as usize * 16 + i];
                    uart::putc(if (0x20..0x7f).contains(&c) { c } else { b'.' });
                }
                kprintln!("|");
            }
        } else {
            kprintln!("blkdump: read failed");
        }
        vfs::fd_close(t, fd);
    }
    vfs::fd_table_destroy(t);
}

fn cmd_blkwrite(sector: u64, text: &str) {
    let mut b = alloc::vec![0u8; 512];
    let n = core::cmp::min(text.len(), 511);
    b[..n].copy_from_slice(&text.as_bytes()[..n]);
    let t = vfs::fd_table_create();
    let fd = vfs::fd_open(t, "/dev/blk");
    if fd >= 0 {
        vfs::fd_seek(t, fd, (sector * 512) as i64, vfs::SEEK_SET);
        let ok = vfs::fd_write(t, fd, &b) == 512;
        kprintln!("blkwrite sector {}: {}", sector, if ok { "ok" } else { "failed" });
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

fn cmd_memtest(kb: u64) {
    use alloc::vec::Vec;
    let n = (kb as usize) * 1024;
    let before = heap::free_bytes();
    let mut v: Vec<u8> = Vec::with_capacity(n);
    for i in 0..n {
        v.push((i & 0xff) as u8);
    }
    let mut ok = true;
    for i in 0..n {
        if v[i] != (i & 0xff) as u8 {
            ok = false;
            break;
        }
    }
    let during = heap::free_bytes();
    drop(v);
    let after = heap::free_bytes();
    kprintln!(
        "memtest {} KiB: {} | free {} -> {} -> {} bytes",
        kb,
        if ok { "PASS" } else { "FAIL" },
        before, during, after
    );
}

fn cmd_sysinfo() {
    kprintln!("   ___ Fermi OS (Rust) ___");
    kprintln!("  OS      : Fermi OS — bare-metal aarch64, pure Rust + asm");
    kprintln!("  uptime  : {} s", timer::uptime_seconds());
    kprintln!("  EL      : {} | timer {} Hz", crate::cpu::current_el(), timer::get_frequency());
    let total = pmm::total_pages();
    let used = pmm::used_pages();
    kprintln!("  memory  : {} / {} MiB pages used, heap {} KiB free",
              used * 4 / 1024, total * 4 / 1024, heap::free_bytes() / 1024);
    let nd = net::render_info();
    for line in nd.lines().take(3) {
        kprintln!("  net     : {}", line);
    }
    kprintln!("  tasks   :");
    kprint!("{}", sched::render_tasks());
}

fn cmd_stat(path: &str) {
    let node = vfs::resolve(path);
    if node.is_null() {
        kprintln!("stat: {}: not found", path);
        return;
    }
    unsafe {
        let kind = match (*node).vtype {
            vfs::VnodeType::Reg => "regular file",
            vfs::VnodeType::Dir => "directory",
            vfs::VnodeType::Chr => "character device",
            vfs::VnodeType::Blk => "block device",
        };
        kprintln!("  File : {}", path);
        kprintln!("  Type : {}", kind);
        kprintln!("  Size : {} bytes", (*node).size);
        if (*node).dev == vfs::DevOps::Fat32File || (*node).is_dir_fat32 {
            kprintln!("  FAT32 first cluster: {}", (*node).private0);
        }
    }
}

fn dispatch(line: &str) {
    let mut parts = line.split_whitespace();
    let cmd = match parts.next() {
        Some(c) => c,
        None => return,
    };
    let arg1 = parts.next().unwrap_or("");
    let arg2 = parts.next().unwrap_or("");
    match cmd {
        "help" => {
            kprintln!("builtins: help uptime version uname date ps top sysinfo free meminfo");
            kprintln!("  diag  : irqs traps cpuinfo heapstat memtest <kb> smp smptest");
            kprintln!("  net   : ifconfig ping [n] arp [ip] resolve <host> http <host> ntp [host]");
            kprintln!("  files : ls [path] cat <path> grep <pat> <path> wc <path> stat <path>");
            kprintln!("          write <name> <text> cp <src> <dst> mv <old> <new> rm <path>");
            kprintln!("          hexdump <path> df run <elf>");
            kprintln!("  blk   : blkdump <sector> blkwrite <sector> <text>");
            kprintln!("  proc  : sleep <ms> kill <pid> echo <text> rand [n] reboot clear");
            kprintln!("          balloon <inflate|deflate|status> [n] vlog <text>");
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
        "date" => kprintln!("{}", crate::rtc::format_now()),
        "history" => {
            let h = unsafe { HISTORY.get() };
            for (i, c) in h.iter().enumerate() {
                kprintln!("{:>3}  {}", i, c);
            }
        }
        "version" => kprintln!("Fermi OS (Rust) — aarch64, rustc 1.85.0"),
        "smpsched" => {
            let k = parse_u64(arg1).unwrap_or(120).max(1);
            crate::smp::pool_seed(k);
            kprintln!("seeded {} pooled tasks; draining on both cores...", k);
            sched::sleep_ms(1500);
            let (r0, r1, s0, s1, seeded, rem) = crate::smp::pool_stats();
            let expect = (1000..1000 + seeded).sum::<u64>();
            kprintln!("  core0 ran {} tasks (pid-sum {})", r0, s0);
            kprintln!("  core1 ran {} tasks (pid-sum {})", r1, s1);
            kprintln!("  total {}/{} run, {} remaining, checksum {} (expect {}) -> {}",
                      r0 + r1, seeded, rem, s0 + s1, expect,
                      if r0 + r1 == seeded && s0 + s1 == expect { "OK" } else { "incomplete" });
        }
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
        "sysinfo" => cmd_sysinfo(),
        "uname" => {
            if arg1 == "-a" {
                kprintln!("Fermi 0.1.0 fermi-os aarch64 ARMv8-A QEMU-virt Rust+asm");
            } else {
                kprintln!("Fermi");
            }
        }
        "ps" => kprint!("{}", sched::render_tasks()),
        "top" => cmd_top(),
        "free" | "meminfo" => cmd_free(),
        "memtest" => cmd_memtest(parse_u64(arg1).unwrap_or(64).clamp(1, 512)),
        "heapstat" => kprint!("{}", heap::render_stats()),
        "df" => {
            let cap = crate::virtio::blk::capacity_sectors();
            kprintln!("/dev/blk : {} sectors ({} MiB)", cap, cap / 2048);
            let (tot, free, bpc) = crate::fs::fat32::usage();
            let used = tot - free;
            kprintln!("/mnt/fat32: {} KiB total, {} KiB used, {} KiB free (clusters {}/{}, {}B each)",
                      tot as u64 * bpc as u64 / 1024, used as u64 * bpc as u64 / 1024,
                      free as u64 * bpc as u64 / 1024, used, tot, bpc);
        }
        "ifconfig" => kprint!("{}", net::render_info()),
        "arp" => {
            if arg1.is_empty() {
                kprint!("{}", net::render_arp());
            } else {
                let mut ip = [0u8; 4];
                let mut ok = true;
                for (i, part) in arg1.split('.').enumerate() {
                    if i >= 4 { ok = false; break; }
                    match parse_u64(part) { Some(v) if v <= 255 => ip[i] = v as u8, _ => { ok = false; } }
                }
                if !ok {
                    kprintln!("usage: arp [a.b.c.d]");
                } else if let Some(m) = net::arp_resolve(&ip) {
                    kprintln!("{}.{}.{}.{} is {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                              ip[0],ip[1],ip[2],ip[3], m[0],m[1],m[2],m[3],m[4],m[5]);
                } else {
                    kprintln!("arp: no reply for {}.{}.{}.{}", ip[0],ip[1],ip[2],ip[3]);
                }
            }
        }
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
        "traps" => kprint!("{}", crate::exception::render_stats()),
        "ls" => {
            let path = if arg1.is_empty() { "/" } else { arg1 };
            kprint!("{}", vfs::list(path));
        }
        "cp" => {
            let dst = parts.next().unwrap_or("");
            if arg1.is_empty() || dst.is_empty() {
                kprintln!("usage: cp <src-path> <dst-name.ext>");
            } else {
                cmd_cp(arg1, dst);
}
}
        "blkdump" => {
            match parse_u64(arg1) { Some(s) => cmd_blkdump(s), None => kprintln!("usage: blkdump <sector>") }
        }
        "blkwrite" => {
            let rest: Vec<&str> = line.splitn(3, ' ').collect();
            match (rest.get(1).and_then(|x| parse_u64(x)), rest.get(2)) {
                (Some(sec), Some(txt)) => cmd_blkwrite(sec, txt),
                _ => kprintln!("usage: blkwrite <sector> <text>"),
            }
}
        "wc" => {
            if arg1.is_empty() { kprintln!("usage: wc <path>"); } else { cmd_wc(arg1); }
}
        "stat" => {
            if arg1.is_empty() { kprintln!("usage: stat <path>"); } else { cmd_stat(arg1); }
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
        "append" => {
            let rest: Vec<&str> = line.splitn(3, ' ').collect();
            if rest.len() < 3 { kprintln!("usage: append <path> <text>"); }
            else { cmd_append(rest[1], rest[2]); }
        }
        "mv" => {
            let rest: Vec<&str> = line.splitn(3, ' ').collect();
            if rest.len() < 3 {
                kprintln!("usage: mv <oldpath> <newpath>");
            } else if vfs::rename(rest[1], rest[2]) {
                kprintln!("renamed {} -> {}", rest[1], rest[2]);
}
}
        "grep" => {
            if arg1.is_empty() || arg2.is_empty() {
                kprintln!("usage: grep <pattern> <path>");
            } else {
                cmd_grep(arg1, arg2);
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
        let trimmed = s.trim();
        history_push(trimmed);
        dispatch(trimmed);
    }
}
