//! EL0 user-space code that lives in the kernel image.
//!
//! Functions here run at EL0 after `sched::create_task` maps the kernel's
//! `.text`/`.rodata` window (RO + EL0-executable) into the task's TTBR0. They
//! must touch ONLY their own stack and the syscall ABI — no kernel statics, no
//! MMIO, no `kprintln!`. All kernel interaction goes through `svc`.

#![allow(dead_code)]

// Syscall numbers (must match src/syscall/mod.rs).
const SYS_READ: u64 = 0;
const SYS_WRITE: u64 = 1;
const SYS_OPEN: u64 = 2;
const SYS_CLOSE: u64 = 3;
const SYS_EXIT: u64 = 4;
const SYS_YIELD: u64 = 5;
const SYS_SLEEP: u64 = 6;
const SYS_GETPID: u64 = 7;
const SYS_UPTIME: u64 = 9;
const SYS_NET_PING: u64 = 10;
const SYS_KILL: u64 = 11;
const SYS_FORK: u64 = 12;
const SYS_EXEC: u64 = 13;
const SYS_BALLOON: u64 = 14;
const SYS_REBOOT: u64 = 15;

// --- raw syscall wrappers ---------------------------------------------------

#[inline(always)]
fn syscall3(num: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    let ret: i64;
    unsafe {
        core::arch::asm!(
            "svc #0",
            in("x8") num,
            inlateout("x0") a0 => ret,
            in("x1") a1,
            in("x2") a2,
            options(nostack),
        );
    }
    ret
}

fn sys_read(fd: i32, buf: *mut u8, len: usize) -> i64 {
    syscall3(SYS_READ, fd as u64, buf as u64, len as u64)
}
fn sys_write(fd: i32, buf: &[u8]) -> i64 {
    syscall3(SYS_WRITE, fd as u64, buf.as_ptr() as u64, buf.len() as u64)
}
fn sys_open(path: &[u8]) -> i64 {
    // path must be NUL-terminated; callers pass byte-string literals.
    syscall3(SYS_OPEN, path.as_ptr() as u64, 0, 0)
}
fn sys_close(fd: i32) -> i64 {
    syscall3(SYS_CLOSE, fd as u64, 0, 0)
}
fn sys_getpid() -> i64 {
    syscall3(SYS_GETPID, 0, 0, 0)
}
fn sys_uptime() -> i64 {
    syscall3(SYS_UPTIME, 0, 0, 0)
}
fn sys_yield() {
    syscall3(SYS_YIELD, 0, 0, 0);
}
fn sys_sleep(ms: u64) {
    syscall3(SYS_SLEEP, ms, 0, 0);
}
fn sys_net_ping(seq: u16) -> i64 {
    syscall3(SYS_NET_PING, seq as u64, 0, 0)
}
fn sys_kill(pid: u64) -> i64 {
    syscall3(SYS_KILL, pid, 0, 0)
}
fn sys_fork() -> i64 {
    syscall3(SYS_FORK, 0, 0, 0)
}
fn sys_exec(path: *const u8, argv: *const *const u8) -> i64 {
    syscall3(SYS_EXEC, path as u64, argv as u64, 0)
}
fn sys_reboot() -> i64 {
    syscall3(SYS_REBOOT, 0, 0, 0)
}
fn sys_balloon(op: u64, n: u64) -> i64 {
    syscall3(SYS_BALLOON, op, n, 0)
}
fn sys_exit() -> ! {
    syscall3(SYS_EXIT, 0, 0, 0);
    loop {
        sys_yield();
    }
}

// --- tiny EL0 helpers (no kernel libs available) ----------------------------

fn print(s: &[u8]) {
    sys_write(1, s);
}

/// Render `v` as decimal into `buf`; returns the written byte count.
fn render_uint(buf: &mut [u8], mut v: u64) -> usize {
    if buf.is_empty() {
        return 0;
    }
    if v == 0 {
        buf[0] = b'0';
        return 1;
    }
    let mut tmp = [0u8; 24];
    let mut n = 0;
    while v > 0 && n < tmp.len() {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    let mut out = 0;
    while n > 0 && out < buf.len() {
        n -= 1;
        buf[out] = tmp[n];
        out += 1;
    }
    out
}

fn print_uint(label: &[u8], v: u64, suffix: &[u8]) {
    let mut buf = [0u8; 64];
    let mut p = 0;
    for &b in label {
        buf[p] = b;
        p += 1;
    }
    p += render_uint(&mut buf[p..], v);
    for &b in suffix {
        buf[p] = b;
        p += 1;
    }
    sys_write(1, &buf[..p]);
}

fn streq(a: &[u8], b: &[u8]) -> bool {
    a == b
}
fn starts_with(s: &[u8], prefix: &[u8]) -> bool {
    s.len() >= prefix.len() && &s[..prefix.len()] == prefix
}
fn atou(s: &[u8]) -> u64 {
    let mut v = 0u64;
    for &c in s {
        if c.is_ascii_digit() {
            v = v * 10 + (c - b'0') as u64;
        } else {
            break;
        }
    }
    v
}

/// Read a line from fd 0 with backspace editing + echo. Returns line length.
fn read_line(buf: &mut [u8]) -> usize {
    let mut n = 0;
    while n < buf.len() - 1 {
        let mut c = 0u8;
        let r = sys_read(0, &mut c as *mut u8, 1);
        if r <= 0 {
            continue;
        }
        if c == b'\r' || c == b'\n' {
            sys_write(1, b"\n");
            break;
        }
        if c == 0x7F || c == 0x08 {
            if n > 0 {
                n -= 1;
                sys_write(1, b"\x08 \x08");
            }
            continue;
        }
        buf[n] = c;
        n += 1;
        sys_write(1, core::slice::from_ref(&c));
    }
    n
}

/// `cat` a file: open, read in chunks to stdout, close.
fn cat(path: &[u8]) {
    let fd = sys_open(path);
    if fd < 0 {
        print(b"cat: cannot open\n");
        return;
    }
    let mut buf = [0u8; 256];
    loop {
        let n = sys_read(fd as i32, buf.as_mut_ptr(), buf.len());
        if n <= 0 {
            break;
        }
        sys_write(1, &buf[..n as usize]);
    }
    sys_close(fd as i32);
}

fn sh_help() {
    print(
        b"Fermi shell built-ins:\n\
          \x20 help            - show this\n\
          \x20 pid             - print my task pid\n\
          \x20 uptime          - print ms since boot\n\
          \x20 ps              - cat /proc/tasks\n\
          \x20 free            - cat /proc/meminfo\n\
          \x20 ifconfig        - cat /proc/netinfo\n\
          \x20 irqs            - cat /proc/interrupts\n\
          \x20 version         - cat /proc/version\n\
          \x20 cpuinfo         - cat /proc/cpuinfo\n\
          \x20 stack           - stress demand-paged user stack growth\n\
          \x20 cat <path>      - print a file\n\
          \x20 hexdump <path>  - hex+ascii dump of a file\n\
          \x20 echo <text>     - print text\n\
          \x20 vlog <text>     - send <text> to /dev/vcons (host log)\n\
          \x20 kill <pid>      - terminate a task by pid\n\
          \x20 fork            - spawn a child task; both print\n\
          \x20 exec <path>     - replace this task with an ELF from disk\n\
          \x20 balloon         - virtio-balloon: status / inflate N / deflate N\n\
          \x20 top             - 5x refresh of tasks/mem/net (1 s)\n\
          \x20 ping            - ICMP echo the slirp gateway (10.0.2.2)\n\
          \x20 sleep <ms>      - block for <ms> milliseconds\n\
          \x20 clear           - clear the terminal (ANSI)\n\
          \x20 reboot          - PSCI SYSTEM_RESET (warm restart)\n\
          \x20 exit            - terminate the shell task\n",
    );
}

/// EL0 interactive shell. Loops reading lines from /dev/console and dispatching
/// built-ins, talking to the kernel only via syscalls.
pub extern "C" fn task_shell() {
    print(b"\nWelcome to the Fermi shell. Type 'help' to start.\n");
    let mut line = [0u8; 128];
    loop {
        print(b"$ ");
        let n = read_line(&mut line);
        if n == 0 {
            continue;
        }
        let cmd = &line[..n];

        if streq(cmd, b"help") {
            sh_help();
        } else if streq(cmd, b"pid") {
            print_uint(b"pid = ", sys_getpid() as u64, b"\n");
        } else if streq(cmd, b"uptime") {
            print_uint(b"uptime = ", sys_uptime() as u64, b" ms\n");
        } else if starts_with(cmd, b"cat ") {
            let mut path = [0u8; 120];
            let plen = copy_cstr(&mut path, &cmd[4..]);
            cat(&path[..plen]);
        } else if streq(cmd, b"ps") {
            cat(b"/proc/tasks\0");
        } else if streq(cmd, b"free") {
            cat(b"/proc/meminfo\0");
        } else if streq(cmd, b"ifconfig") {
            cat(b"/proc/netinfo\0");
        } else if streq(cmd, b"irqs") {
            cat(b"/proc/interrupts\0");
        } else if streq(cmd, b"cpuinfo") {
            cat(b"/proc/cpuinfo\0");
        } else if streq(cmd, b"version") {
            cat(b"/proc/version\0");
        } else if streq(cmd, b"ping") {
            let ttl = sys_net_ping(101);
            if ttl < 0 {
                print(b"ping: no reply\n");
            } else {
                print_uint(b"reply from 10.0.2.2 ttl=", ttl as u64, b"\n");
            }
        } else if streq(cmd, b"top") {
            for _ in 0..5 {
                print(b"\x1b[2J\x1b[H=== Fermi top ===\n\n");
                cat(b"/proc/tasks\0");
                print(b"\n");
                cat(b"/proc/meminfo\0");
                print(b"\n");
                cat(b"/proc/netinfo\0");
                sys_sleep(1000);
            }
            print(b"(top finished)\n");
        } else if streq(cmd, b"fork") {
            let r = sys_fork();
            // fork's three branches (child / error / parent) read more clearly
            // as a comparison chain than a match on a signed value.
            #[allow(clippy::comparison_chain)]
            if r == 0 {
                print(b"[fork-child] hello from the child!\n");
                print_uint(b"[fork-child] my pid=", sys_getpid() as u64, b"\n");
                sys_exit();
            } else if r < 0 {
                print(b"fork: failed\n");
            } else {
                print_uint(b"fork: child pid=", r as u64, b"\n");
            }
        } else if streq(cmd, b"balloon") || starts_with(cmd, b"balloon ") {
            let arg = if cmd.len() > 8 { &cmd[8..] } else { b"" as &[u8] };
            if arg.is_empty() || streq(arg, b"status") {
                let a = sys_balloon(2, 0);
                let t = sys_balloon(3, 0);
                print_uint(b"balloon: actual=", a as u64, b" pages");
                print_uint(b" target=", t as u64, b" pages\n");
            } else if starts_with(arg, b"inflate ") {
                let got = sys_balloon(0, atou(&arg[8..]));
                print_uint(b"balloon: inflated ", if got < 0 { 0 } else { got as u64 }, b" pages\n");
            } else if starts_with(arg, b"deflate ") {
                let got = sys_balloon(1, atou(&arg[8..]));
                print_uint(b"balloon: deflated ", if got < 0 { 0 } else { got as u64 }, b" pages\n");
            } else {
                print(b"balloon: usage: balloon [status|inflate N|deflate N]\n");
            }
        } else if starts_with(cmd, b"kill ") {
            let pid = atou(&cmd[5..]);
            if sys_kill(pid) == 0 {
                print(b"killed.\n");
            } else {
                print(b"kill: no such pid (or pid is idle)\n");
            }
        } else if starts_with(cmd, b"exec ") {
            // Tokenize the tail in-place: NUL-terminate each word and record
            // argv pointers. On success exec never returns (the image is
            // replaced); on failure it returns -1.
            let mut argv: [*const u8; 16] = [core::ptr::null(); 16];
            let mut argc = 0usize;
            let tail = &mut line[5..n + 1]; // include the NUL slot at [n]
            let mut i = 0usize;
            let len = tail.len();
            while i < len && argc < 15 {
                while i < len && tail[i] == b' ' {
                    i += 1;
                }
                if i >= len || tail[i] == 0 {
                    break;
                }
                argv[argc] = tail[i..].as_ptr();
                argc += 1;
                while i < len && tail[i] != b' ' && tail[i] != 0 {
                    i += 1;
                }
                if i < len && tail[i] == b' ' {
                    tail[i] = 0; // terminate this word
                    i += 1;
                }
            }
            if argc == 0 {
                print(b"exec: usage: exec <path> [args...]\n");
            } else {
                let r = sys_exec(argv[0], argv.as_ptr());
                if r < 0 {
                    print(b"exec: failed (open / read / alloc)\n");
                }
            }
        } else if starts_with(cmd, b"echo ") {
            print(&cmd[5..]);
            print(b"\n");
        } else if streq(cmd, b"clear") {
            print(b"\x1b[2J\x1b[H");
        } else if starts_with(cmd, b"sleep ") {
            sys_sleep(atou(&cmd[6..]));
        } else if streq(cmd, b"stack") {
            // Force demand-paged stack growth: a 64 KiB local touched per page.
            let mut buf = [0u8; 64 * 1024];
            let mut i = 0;
            while i < buf.len() {
                unsafe { core::ptr::write_volatile(&mut buf[i], (i & 0xFF) as u8) };
                i += 4096;
            }
            let mut ok = true;
            i = 0;
            while i < buf.len() {
                if unsafe { core::ptr::read_volatile(&buf[i]) } != (i & 0xFF) as u8 {
                    ok = false;
                }
                i += 4096;
            }
            print(if ok {
                b"stack: 64 KiB stack probe OK (16 page-grows)\n" as &[u8]
            } else {
                b"stack: 64 KiB stack probe FAILED\n"
            });
        } else if starts_with(cmd, b"hexdump ") {
            let mut path = [0u8; 120];
            let plen = copy_cstr(&mut path, &cmd[8..]);
            hexdump(&path[..plen]);
        } else if starts_with(cmd, b"vlog ") {
            // Send the rest of the line to /dev/vcons (virtio-console host log).
            let fd = sys_open(b"/dev/vcons\0");
            if fd < 0 {
                print(b"vlog: cannot open /dev/vcons\n");
            } else {
                sys_write(fd as i32, &cmd[5..]);
                sys_write(fd as i32, b"\n");
                sys_close(fd as i32);
                print(b"vlog: sent (check the virtio-console host file)\n");
            }
        } else if streq(cmd, b"reboot") {
            print(b"rebooting via PSCI SYSTEM_RESET...\n");
            // Reboot is a syscall: HVC/SMC must come from EL1, and undefined-
            // from-EL0 would just fault. On a QEMU virt machine without a PSCI
            // conduit (no EL2/EL3) this returns -1 and we say so.
            sys_reboot();
            print(b"reboot: PSCI unavailable on this machine\n");
        } else if streq(cmd, b"exit") {
            print(b"bye!\n");
            sys_exit();
        } else {
            print(b"unknown command - try 'help'\n");
        }
    }
}

/// hexdump a file: 8-hex-digit offset, 16 bytes hex, then ASCII gutter.
fn hexdump(path: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let fd = sys_open(path);
    if fd < 0 {
        print(b"hexdump: cannot open\n");
        return;
    }
    let mut hbuf = [0u8; 16];
    let mut offset: u64 = 0;
    loop {
        let got = sys_read(fd as i32, hbuf.as_mut_ptr(), 16);
        if got <= 0 {
            break;
        }
        let got = got as usize;
        let mut ln = [0u8; 96];
        let mut p = 0;
        for i in (0..8).rev() {
            ln[p] = HEX[((offset >> (i * 4)) & 0xF) as usize];
            p += 1;
        }
        ln[p] = b' ';
        ln[p + 1] = b' ';
        p += 2;
        for i in 0..16 {
            if i < got {
                ln[p] = HEX[(hbuf[i] >> 4) as usize];
                ln[p + 1] = HEX[(hbuf[i] & 0xF) as usize];
            } else {
                ln[p] = b' ';
                ln[p + 1] = b' ';
            }
            p += 2;
            ln[p] = b' ';
            p += 1;
            if i == 7 {
                ln[p] = b' ';
                p += 1;
            }
        }
        ln[p] = b'|';
        p += 1;
        for i in 0..got {
            let c = hbuf[i];
            ln[p] = if (32..127).contains(&c) { c } else { b'.' };
            p += 1;
        }
        ln[p] = b'|';
        ln[p + 1] = b'\n';
        p += 2;
        sys_write(1, &ln[..p]);
        offset += got as u64;
    }
    sys_close(fd as i32);
}

/// Copy `src` into `dst` and NUL-terminate; returns total length incl. NUL.
fn copy_cstr(dst: &mut [u8], src: &[u8]) -> usize {
    let n = core::cmp::min(src.len(), dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
    n + 1
}

/// EL0 task that deliberately faults (unmapped write) to exercise the
/// kill-on-fault path; the kernel should kill only this task.
pub extern "C" fn task_crash() {
    print(b"[Task C] about to deref a bad pointer at 0x12345678 (expect kill)\n");
    unsafe {
        core::ptr::write_volatile(0x1234_5678 as *mut u64, 0xDEAD_BEEF_CAFE_BABE);
    }
    print(b"[Task C] !!! continued past fault !!!\n");
    sys_exit();
}

/// Original simple EL0 demo task (kept for the boot smoke test).
pub extern "C" fn task_user() {
    print(b"[user] hello from EL0 via SYS_WRITE!\n");
    print_uint(b"[user] my pid is ", sys_getpid() as u64, b"\n");
    for i in 0..3u64 {
        print_uint(b"[user] iteration ", i, b"");
        print_uint(b", uptime_ms=", sys_uptime() as u64, b"\n");
        sys_yield();
    }
    print(b"[user] done, calling SYS_EXIT\n");
    sys_exit();
}
