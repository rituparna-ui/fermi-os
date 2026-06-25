//! Built-in device nodes registered into the VFS:
//!   /dev/console — PL011 UART (interactive stdin/stdout/stderr)
//!   /dev/null    — discard / EOF
//!   /dev/zero    — zero-fill / discard
//!   /dev/rng     — virtio-rng entropy
//!   /dev/vcons   — virtio-console TX side-channel
//!   /dev/blk     — raw virtio-blk (sector-aligned byte offsets)
//!
//! Each device supplies a `FileOperations` vtable the VFS dispatches through.

use crate::drivers::virtio::{blk, console, rng};
use crate::fs::vfs::{self, File, FileOperations, Vnode};
use crate::klib::uart::Uart;

const SECTOR: usize = 512;

// --- /dev/console (PL011 UART) ----------------------------------------------

fn console_read(_n: *mut Vnode, _f: *mut File, buf: *mut u8, count: usize) -> i64 {
    let uart = Uart;
    for i in 0..count {
        unsafe { buf.add(i).write(uart.getc()) };
    }
    count as i64
}

fn console_write(_n: *mut Vnode, _f: *mut File, buf: *const u8, count: usize) -> i64 {
    // Write atomically so a task's console output isn't byte-interleaved with an
    // IRQ-context kprintln!.
    let slice = unsafe { core::slice::from_raw_parts(buf, count) };
    Uart.write_locked(slice);
    count as i64
}

static CONSOLE_OPS: FileOperations = FileOperations {
    read: Some(console_read),
    write: Some(console_write),
};

// --- /dev/null --------------------------------------------------------------

fn null_read(_n: *mut Vnode, _f: *mut File, _buf: *mut u8, _count: usize) -> i64 {
    0 // EOF
}
fn null_write(_n: *mut Vnode, _f: *mut File, _buf: *const u8, count: usize) -> i64 {
    count as i64 // accept + discard
}
static NULL_OPS: FileOperations = FileOperations {
    read: Some(null_read),
    write: Some(null_write),
};

// --- /dev/zero --------------------------------------------------------------

fn zero_read(_n: *mut Vnode, _f: *mut File, buf: *mut u8, count: usize) -> i64 {
    unsafe { core::ptr::write_bytes(buf, 0, count) };
    count as i64
}
fn zero_write(_n: *mut Vnode, _f: *mut File, _buf: *const u8, count: usize) -> i64 {
    count as i64
}
static ZERO_OPS: FileOperations = FileOperations {
    read: Some(zero_read),
    write: Some(zero_write),
};

// --- /dev/rng ---------------------------------------------------------------

fn rng_read(_n: *mut Vnode, _f: *mut File, buf: *mut u8, count: usize) -> i64 {
    let slice = unsafe { core::slice::from_raw_parts_mut(buf, count) };
    rng::read(slice) as i64
}
static RNG_OPS: FileOperations = FileOperations {
    read: Some(rng_read),
    write: None,
};

// --- /dev/vcons (virtio-console TX) -----------------------------------------

fn vcons_read(_n: *mut Vnode, _f: *mut File, _buf: *mut u8, _count: usize) -> i64 {
    0 // RX not posted
}
fn vcons_write(_n: *mut Vnode, _f: *mut File, buf: *const u8, count: usize) -> i64 {
    let slice = unsafe { core::slice::from_raw_parts(buf, count) };
    console::send(slice)
}
static VCONS_OPS: FileOperations = FileOperations {
    read: Some(vcons_read),
    write: Some(vcons_write),
};

// --- /dev/blk (raw block device, sector-aligned) ----------------------------

fn blk_read(_n: *mut Vnode, f: *mut File, buf: *mut u8, count: usize) -> i64 {
    let offset = unsafe { (*f).offset };
    if offset % SECTOR as i64 != 0 || count % SECTOR != 0 {
        return -1;
    }
    let sectors = count / SECTOR;
    let base_sector = offset as u64 / SECTOR as u64;
    for i in 0..sectors {
        let dst = unsafe { core::slice::from_raw_parts_mut(buf.add(i * SECTOR), SECTOR) };
        if !blk::read(base_sector + i as u64, dst) {
            return -1;
        }
    }
    unsafe { (*f).offset += count as i64 };
    count as i64
}

fn blk_write(_n: *mut Vnode, f: *mut File, buf: *const u8, count: usize) -> i64 {
    let offset = unsafe { (*f).offset };
    if offset % SECTOR as i64 != 0 || count % SECTOR != 0 {
        return -1;
    }
    let sectors = count / SECTOR;
    let base_sector = offset as u64 / SECTOR as u64;
    for i in 0..sectors {
        let src = unsafe { core::slice::from_raw_parts(buf.add(i * SECTOR), SECTOR) };
        if !blk::write(base_sector + i as u64, src) {
            return -1;
        }
    }
    unsafe { (*f).offset += count as i64 };
    count as i64
}

static BLK_OPS: FileOperations = FileOperations {
    read: Some(blk_read),
    write: Some(blk_write),
};

/// Register all built-in device nodes into the VFS.
pub fn register() {
    vfs::register_chardev("console", &CONSOLE_OPS);
    vfs::register_chardev("null", &NULL_OPS);
    vfs::register_chardev("zero", &ZERO_OPS);
    vfs::register_chardev("rng", &RNG_OPS);
    vfs::register_chardev("vcons", &VCONS_OPS);
    vfs::register_blockdev("blk", &BLK_OPS);
}
