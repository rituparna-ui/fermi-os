//! System-call interface (SVC #0 dispatch).
//!
//! Port of `src/syscall/syscall.c` (core subset). AAPCS64 convention: x8 holds
//! the syscall number, x0..x5 the arguments, and the return value is written
//! back into the trap frame's x0. User pointers are range-checked against
//! `[0, USER_STACK_TOP)` to prevent kernel-pointer injection.

use crate::exception::{timer, TrapFrame};
use crate::kprintln;
use crate::mm::mmu::USER_STACK_TOP;
use crate::sched;
use crate::uart;

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

/// Validate that a user buffer lies wholly within the EL0 address window.
fn user_ptr_ok(ptr: u64, len: u64) -> bool {
    ptr != 0 && ptr.checked_add(len).map_or(false, |end| end <= USER_STACK_TOP)
}

/// Write `len` bytes from user VA `buf` to the console (fd 1/2).
fn sys_write(fd: u64, buf: u64, len: u64) -> i64 {
    if fd != 1 && fd != 2 {
        return -1;
    }
    if !user_ptr_ok(buf, len) {
        kprintln!("[SYS] write: bad user ptr {:#x}+{}", buf, len);
        return -1;
    }
    // TTBR0 still points at the calling task's user table, so the kernel can
    // read the EL0 buffer directly.
    for i in 0..len {
        let b = unsafe { *((buf + i) as *const u8) };
        uart::putc(b);
    }
    len as i64
}

pub fn syscall_dispatch(frame: &mut TrapFrame) {
    let num = frame.regs[8];
    let a0 = frame.regs[0];
    let a1 = frame.regs[1];
    let a2 = frame.regs[2];

    let ret: i64 = match num {
        SYS_WRITE => sys_write(a0, a1, a2),
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
        SYS_EXIT => {
            kprintln!("[SYS] exit({})", a0 as i64);
            sched::task_exit(); // does not return
            0
        }
        _ => {
            kprintln!("[SYS] unknown syscall {} (ENOSYS)", num);
            -1
        }
    };

    frame.regs[0] = ret as u64;
}
