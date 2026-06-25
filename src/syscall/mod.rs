//! System-call dispatch (SVC #0 from EL0).
//!
//! AAPCS64 convention: x8 = syscall number, x0..x7 = args, return in x0 (written
//! back into the trap frame). User pointers are range-checked against
//! [0, USER_STACK_TOP) to close the kernel-pointer-injection hole. IRQs are
//! unmasked early so long/blocking syscalls don't starve other tasks.

#![allow(dead_code)]

use crate::drivers::virtio::balloon;
use crate::exception::TrapFrame;
use crate::fs::vfs;
use crate::kprintln;
use crate::mm::consts::USER_STACK_TOP;
use crate::sched;

mod exec;

// Syscall numbers (x8). Frozen ABI — must match the user-side wrappers.
pub const SYS_READ: u64 = 0;
pub const SYS_WRITE: u64 = 1;
pub const SYS_OPEN: u64 = 2;
pub const SYS_CLOSE: u64 = 3;
pub const SYS_EXIT: u64 = 4;
pub const SYS_YIELD: u64 = 5;
pub const SYS_SLEEP: u64 = 6;
pub const SYS_GETPID: u64 = 7;
pub const SYS_LSEEK: u64 = 8;
pub const SYS_UPTIME: u64 = 9;
pub const SYS_NET_PING: u64 = 10;
pub const SYS_KILL: u64 = 11;
pub const SYS_FORK: u64 = 12;
pub const SYS_EXEC: u64 = 13;
pub const SYS_BALLOON: u64 = 14;
/// Reboot via PSCI from EL1 (HVC from EL0 traps as undefined). Extends the
/// original ABI — the C shell issued HVC directly from EL0, which faults on a
/// QEMU virt machine without EL2/EL3, so reboot never worked there.
pub const SYS_REBOOT: u64 = 15;
/// readdir(path, index, name_buf): copy the index-th entry name of the
/// directory at `path` into the user buffer (cap 256). Returns name length, or
/// -1 past the end / not a directory. Extends the original ABI.
pub const SYS_READDIR: u64 = 16;

pub const BALLOON_OP_INFLATE: u64 = 0;
pub const BALLOON_OP_DEFLATE: u64 = 1;
pub const BALLOON_OP_ACTUAL: u64 = 2;
pub const BALLOON_OP_TARGET: u64 = 3;

const USER_PATH_MAX: u64 = 4096;

/// True if [ptr, ptr+len) lies entirely in the user range. Zero-length always ok.
pub fn user_buf_ok(ptr: u64, len: u64) -> bool {
    if len == 0 {
        return true;
    }
    // checked_add rejects the overflow case (a hostile ptr+len that wraps);
    // a C-style `ptr+len < ptr` guard would panic on overflow in a debug build.
    match ptr.checked_add(len) {
        Some(end) => end <= USER_STACK_TOP,
        None => false,
    }
}

/// Validate a NUL-terminated user string; returns its length or -1.
pub fn user_str_ok(ptr: u64) -> i64 {
    if ptr >= USER_STACK_TOP {
        return -1;
    }
    let bound = core::cmp::min(USER_STACK_TOP - ptr, USER_PATH_MAX);
    // SAFETY: ptr is in the user range; we scan at most `bound` bytes. An
    // unmapped in-range page would fault here (documented limitation).
    unsafe {
        let s = ptr as *const u8;
        for i in 0..bound {
            if s.add(i as usize).read() == 0 {
                return i as i64;
            }
        }
    }
    -1
}

/// Interpret a user VA + len as a string slice (caller must have validated).
fn user_str<'a>(ptr: u64) -> Option<&'a str> {
    let len = user_str_ok(ptr);
    if len < 0 {
        return None;
    }
    // SAFETY: validated range + NUL within bound.
    let bytes = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
    core::str::from_utf8(bytes).ok()
}

/// SVC dispatch entry, called from the exception handler with the trap frame.
pub fn dispatch(frame: &mut TrapFrame) {
    // Unmask IRQs so long syscalls (e.g. blocking UART read) are preemptible.
    unsafe { core::arch::asm!("msr daifclr, #2", options(nomem, nostack)) };

    let num = frame.regs[8];
    let arg0 = frame.regs[0];
    let arg1 = frame.regs[1];
    let arg2 = frame.regs[2];

    let fds = unsafe { (*sched::current()).fds };
    let mut ret: i64 = -1;

    match num {
        SYS_READ => {
            if !fds.is_null() && user_buf_ok(arg1, arg2) {
                ret = vfs::fd_read(fds, arg0 as i32, arg1 as *mut u8, arg2 as usize);
            } else {
                crate::klib::uart::Uart.errorln("[SYSCALL] SYS_READ rejected: bad user buffer");
            }
        }
        SYS_WRITE => {
            if !fds.is_null() && user_buf_ok(arg1, arg2) {
                ret = vfs::fd_write(fds, arg0 as i32, arg1 as *const u8, arg2 as usize);
            } else {
                crate::klib::uart::Uart.errorln("[SYSCALL] SYS_WRITE rejected: bad user buffer");
            }
        }
        SYS_OPEN => {
            if !fds.is_null() {
                if let Some(path) = user_str(arg0) {
                    ret = vfs::fd_open(fds, path) as i64;
                } else {
                    crate::klib::uart::Uart.errorln("[SYSCALL] SYS_OPEN rejected: bad user path");
                }
            }
        }
        SYS_CLOSE => {
            if !fds.is_null() {
                ret = vfs::fd_close(fds, arg0 as i32) as i64;
            }
        }
        SYS_EXIT => {
            sched::task_exit();
        }
        SYS_YIELD => {
            sched::schedule();
            ret = 0;
        }
        SYS_SLEEP => {
            sched::sleep_ms(arg0);
            ret = 0;
        }
        SYS_GETPID => {
            ret = unsafe { (*sched::current()).pid as i64 };
        }
        SYS_LSEEK => {
            if !fds.is_null() {
                ret = vfs::fd_seek(fds, arg0 as i32, arg1 as i64, arg2 as i32);
            }
        }
        SYS_UPTIME => {
            ret = crate::exception::timer::uptime_ms() as i64;
        }
        SYS_NET_PING => {
            ret = crate::drivers::virtio::net::send_ping(arg0 as u16);
        }
        SYS_KILL => {
            ret = sched::kill_task(arg0);
        }
        SYS_FORK => {
            ret = sched::fork(sched::current(), frame);
        }
        SYS_REBOOT => {
            ret = crate::arch::cpu::reboot();
        }
        SYS_READDIR => {
            // arg0 = path (user str), arg1 = index, arg2 = name buffer (>= 256).
            const NAME_CAP: u64 = 256;
            if user_str(arg0).is_some() && user_buf_ok(arg2, NAME_CAP) {
                let path = user_str(arg0).unwrap();
                let dir = vfs::resolve(path);
                ret = vfs::readdir(dir, arg1 as usize, arg2 as *mut u8, NAME_CAP as usize);
            } else {
                crate::klib::uart::Uart.errorln("[SYSCALL] SYS_READDIR rejected: bad args");
            }
        }
        SYS_BALLOON => {
            ret = match arg0 {
                BALLOON_OP_INFLATE => balloon::inflate(arg1 as u32),
                BALLOON_OP_DEFLATE => balloon::deflate(arg1 as u32),
                BALLOON_OP_ACTUAL => balloon::status().0 as i64,
                BALLOON_OP_TARGET => balloon::status().1 as i64,
                _ => -1,
            };
        }
        SYS_EXEC => {
            // On success the frame is fully rewritten; return early without
            // clobbering x0 (which now holds argc).
            if exec::sys_exec(arg0, arg1, frame) >= 0 {
                return;
            }
            ret = -1;
        }
        _ => {
            kprintln!("[SYSCALL] Unknown syscall {}", num);
            ret = -1;
        }
    }

    frame.regs[0] = ret as u64;
}
