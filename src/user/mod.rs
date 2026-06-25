//! EL0 user-space code that lives in the kernel image.
//!
//! Functions here run at EL0 after `sched::create_task` maps the kernel's
//! `.text`/`.rodata` window (RO + EL0-executable) into the task's TTBR0. They
//! must therefore touch ONLY their own stack and the syscall ABI — no kernel
//! statics, no MMIO, no `kprintln!`. All kernel interaction goes through `svc`.

#![allow(dead_code)]

// Syscall numbers (must match src/syscall/mod.rs).
const SYS_WRITE: u64 = 1;
const SYS_EXIT: u64 = 4;
const SYS_YIELD: u64 = 5;
const SYS_GETPID: u64 = 7;
const SYS_UPTIME: u64 = 9;

/// `svc #0` with x8 = number, x0..x2 = args; returns x0.
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

fn sys_write(fd: i32, buf: &[u8]) -> i64 {
    syscall3(SYS_WRITE, fd as u64, buf.as_ptr() as u64, buf.len() as u64)
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
fn sys_exit() -> ! {
    syscall3(SYS_EXIT, 0, 0, 0);
    // SYS_EXIT never returns; loop as a safety net.
    loop {
        sys_yield();
    }
}

/// Format a u64 as decimal into `buf`, returning the byte slice written.
fn fmt_u64(buf: &mut [u8; 20], mut v: u64) -> &[u8] {
    if v == 0 {
        buf[0] = b'0';
        return &buf[..1];
    }
    let mut tmp = [0u8; 20];
    let mut i = 0;
    while v != 0 {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    // reverse
    for j in 0..i {
        buf[j] = tmp[i - 1 - j];
    }
    &buf[..i]
}

/// EL0 test task: greets via SYS_WRITE, reports pid + uptime, yields a few
/// times, then exits — all through the syscall ABI.
pub extern "C" fn task_user() {
    sys_write(1, b"[user] hello from EL0 via SYS_WRITE!\n");

    let pid = sys_getpid();
    let mut nbuf = [0u8; 20];
    sys_write(1, b"[user] my pid is ");
    sys_write(1, fmt_u64(&mut nbuf, pid as u64));
    sys_write(1, b"\n");

    for i in 0..3u64 {
        let up = sys_uptime();
        sys_write(1, b"[user] iteration ");
        let mut b2 = [0u8; 20];
        sys_write(1, fmt_u64(&mut b2, i));
        sys_write(1, b", uptime_ms=");
        let mut b3 = [0u8; 20];
        sys_write(1, fmt_u64(&mut b3, up as u64));
        sys_write(1, b"\n");
        sys_yield();
    }

    sys_write(1, b"[user] done, calling SYS_EXIT\n");
    sys_exit();
}
