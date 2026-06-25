//! System-call interface (SVC #0 dispatch).
//!
//! Port of `src/syscall/syscall.c` (core subset). x8 = syscall number,
//! x0..x5 = args, return value in x0. I/O routes through the current task's
//! fd table. User pointers are range-checked against `[0, USER_STACK_TOP)`.

use crate::exception::{timer, TrapFrame};
use crate::fs::vfs::{self, FdTable};
use crate::kprintln;
use crate::mm::mmu::USER_STACK_TOP;
use crate::sched;
use crate::strings;

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

fn user_ptr_ok(ptr: u64, len: u64) -> bool {
    ptr != 0 && ptr.checked_add(len).map_or(false, |end| end <= USER_STACK_TOP)
}

fn current_fds() -> *mut FdTable {
    unsafe { (*sched::current()).fds as *mut FdTable }
}

pub fn syscall_dispatch(frame: &mut TrapFrame) {
    let num = frame.regs[8];
    let a0 = frame.regs[0];
    let a1 = frame.regs[1];
    let a2 = frame.regs[2];
    let a3 = frame.regs[3];

    let ret: i64 = match num {
        SYS_WRITE => {
            // a0 = fd, a1 = buf, a2 = len
            if !user_ptr_ok(a1, a2) {
                -1
            } else {
                let buf = unsafe { core::slice::from_raw_parts(a1 as *const u8, a2 as usize) };
                vfs::fd_write(current_fds(), a0 as i32, buf) as i64
            }
        }
        SYS_READ => {
            if !user_ptr_ok(a1, a2) {
                -1
            } else {
                let buf = unsafe { core::slice::from_raw_parts_mut(a1 as *mut u8, a2 as usize) };
                vfs::fd_read(current_fds(), a0 as i32, buf) as i64
            }
        }
        SYS_OPEN => {
            // a0 = path (user C-string)
            if !user_ptr_ok(a0, 1) {
                -1
            } else {
                match unsafe { strings::cstr_as_str(a0 as *const u8) } {
                    Some(path) => vfs::fd_open(current_fds(), path) as i64,
                    None => -1,
                }
            }
        }
        SYS_CLOSE => vfs::fd_close(current_fds(), a0 as i32) as i64,
        SYS_LSEEK => vfs::fd_seek(current_fds(), a0 as i32, a1 as i64, a2 as i64),
        SYS_GETPID => unsafe { (*sched::current()).pid as i64 },
        SYS_YIELD => {
            sched::schedule();
            0
        }
        SYS_SLEEP => {
            sched::sleep_ms(a0);
            0
        }
        SYS_UPTIME => timer::uptime_ms() as i64,
        SYS_NET_PING => crate::net::ping(a0 as u16),
        SYS_KILL => sched::kill(a0 as u64),
        SYS_FORK => sched::fork(frame as *mut TrapFrame),
        SYS_EXEC => {
            // a0 = path (user C-string). Read the ELF from the filesystem.
            if !user_ptr_ok(a0, 1) {
                -1
            } else if let Some(path) = unsafe { strings::cstr_as_str(a0 as *const u8) } {
                let node = vfs::resolve(path);
                if node.is_null() {
                    -1
                } else {
                    let t = vfs::fd_table_create();
                    let fd = vfs::fd_open(t, path);
                    let mut data: alloc::vec::Vec<u8> = alloc::vec::Vec::new();
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
                    if data.is_empty() {
                        -1
                    } else {
                        sched::exec_image(frame as *mut TrapFrame, &data)
                    }
                }
            } else {
                -1
            }
        }
        SYS_EXIT => {
            kprintln!("[SYS] exit({})", a0 as i64);
            sched::task_exit(); // does not return
            0
        }
        _ => {
            kprintln!("[SYS] unknown syscall {} (ENOSYS)", num);
            let _ = a3;
            -1
        }
    };

    frame.regs[0] = ret as u64;
}
