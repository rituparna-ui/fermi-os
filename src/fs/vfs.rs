//! Virtual filesystem: vnode tree, path resolution, per-process fd table.
//!
//! Port of `src/fs/vfs/vfs.c` + `src/devices/devices.c`. Vnodes are heap-
//! allocated and linked with raw pointers (single-threaded kernel). Device
//! read/write is dispatched through the `DevOps` enum (the Rust equivalent of
//! the C `file_operations` vtable).

use crate::kprintln;
use crate::mm::heap::{kfree, kmalloc};
use crate::uart;
use crate::virtio;

pub const MAX_FDS: usize = 64;
pub const SEEK_SET: i64 = 0;
pub const SEEK_CUR: i64 = 1;
pub const SEEK_END: i64 = 2;
const SECTOR: usize = 512;

#[derive(Clone, Copy, PartialEq)]
pub enum VnodeType {
    Reg,
    Dir,
    Chr,
    Blk,
}

/// Device behaviour for a vnode (replaces the C file_operations vtable).
#[derive(Clone, Copy, PartialEq)]
pub enum ProcKind {
    Uptime,
    Meminfo,
    Tasks,
    Interrupts,
    Netinfo,
    Cmdline,
    Version,
}

#[derive(Clone, Copy, PartialEq)]
pub enum DevOps {
    None,
    Console,
    Null,
    Zero,
    Rng,
    Blk,
    Vcons,
    /// FAT32 regular file: private = (first_cluster, size) read via fat32.
    Fat32File,
    /// /proc synthetic file regenerated per read.
    Proc(ProcKind),
}

#[repr(C)]
pub struct Vnode {
    pub name: [u8; 64],
    pub vtype: VnodeType,
    pub dev: DevOps,
    pub size: u64,
    pub private0: u64, // fs-specific (FAT32 first cluster)
    pub is_dir_fat32: bool,
    pub parent: *mut Vnode,
    pub children: *mut Vnode,
    pub next: *mut Vnode,
}

static mut ROOT: *mut Vnode = core::ptr::null_mut();

fn root() -> *mut Vnode {
    unsafe { ROOT }
}

fn alloc_vnode(name: &str, vtype: VnodeType) -> *mut Vnode {
    let v = kmalloc(core::mem::size_of::<Vnode>()) as *mut Vnode;
    if v.is_null() {
        return v;
    }
    unsafe {
        core::ptr::write_bytes(v as *mut u8, 0, core::mem::size_of::<Vnode>());
        let bytes = name.as_bytes();
        let n = core::cmp::min(bytes.len(), 63);
        (*v).name[..n].copy_from_slice(&bytes[..n]);
        (*v).vtype = vtype;
        (*v).dev = DevOps::None;
    }
    v
}

pub fn init() {
    unsafe {
        ROOT = alloc_vnode("/", VnodeType::Dir);
    }
    uart::println("[VFS] Initialized");
}

pub fn create_node(parent: *mut Vnode, name: &str, vtype: VnodeType) -> *mut Vnode {
    let n = alloc_vnode(name, vtype);
    if n.is_null() {
        return n;
    }
    unsafe {
        (*n).parent = parent;
        if !parent.is_null() {
            (*n).next = (*parent).children;
            (*parent).children = n;
        }
    }
    n
}

fn name_eq(node_name: &[u8; 64], s: &[u8]) -> bool {
    for (i, &b) in s.iter().enumerate() {
        if node_name[i] != b {
            return false;
        }
    }
    node_name[s.len()] == 0
}

fn find_child(dir: *mut Vnode, name: &[u8]) -> *mut Vnode {
    unsafe {
        let mut c = (*dir).children;
        while !c.is_null() {
            if name_eq(&(*c).name, name) {
                return c;
            }
            c = (*c).next;
        }
        // Not cached — query FAT32 if this is a FAT32 directory.
        if (*dir).is_dir_fat32 {
            return crate::fs::fat32::lookup(dir, name);
        }
    }
    core::ptr::null_mut()
}

pub fn resolve(path: &str) -> *mut Vnode {
    let bytes = path.as_bytes();
    if bytes.is_empty() || bytes[0] != b'/' {
        return core::ptr::null_mut();
    }
    let mut cur = root();
    let mut i = 1;
    while i < bytes.len() {
        while i < bytes.len() && bytes[i] == b'/' {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }
        let start = i;
        while i < bytes.len() && bytes[i] != b'/' {
            i += 1;
        }
        let comp = &bytes[start..i];
        unsafe {
            if (*cur).vtype != VnodeType::Dir {
                return core::ptr::null_mut();
            }
        }
        if comp == b"." {
            // stay
        } else if comp == b".." {
            unsafe {
                if !(*cur).parent.is_null() {
                    cur = (*cur).parent;
                }
            }
        } else {
            let child = find_child(cur, comp);
            if child.is_null() {
                return core::ptr::null_mut();
            }
            cur = child;
        }
    }
    cur
}

/// Unlink (delete) a FAT32-backed regular file: remove its cached vnode and
/// free it on disk. Returns false if the path is not a deletable FAT32 file.
pub fn unlink(path: &str) -> bool {
    let node = resolve(path);
    if node.is_null() {
        return false;
    }
    unsafe {
        if (*node).dev != DevOps::Fat32File {
            return false; // only FAT32 regular files are deletable
        }
        // Unlink the cached vnode from its parent's child list.
        let parent = (*node).parent;
        if !parent.is_null() {
            let mut c = (*parent).children;
            if c == node {
                (*parent).children = (*node).next;
            } else {
                while !c.is_null() && (*c).next != node {
                    c = (*c).next;
                }
                if !c.is_null() {
                    (*c).next = (*node).next;
                }
            }
        }
    }
    // Derive the final path component as the on-disk name.
    let name = path.rsplit('/').next().unwrap_or("");
    crate::fs::fat32::delete(name.as_bytes())
}

/// List a directory: in-memory children plus on-disk entries for FAT32 dirs.
pub fn list(path: &str) -> alloc::string::String {
    use core::fmt::Write;
    let mut out = alloc::string::String::new();
    let dir = resolve(path);
    if dir.is_null() {
        let _ = writeln!(out, "ls: {}: not found", path);
        return out;
    }
    unsafe {
        if (*dir).vtype != VnodeType::Dir {
            let _ = writeln!(out, "{}", path);
            return out;
        }
        if (*dir).is_dir_fat32 {
            return crate::fs::fat32::list_dir((*dir).private0 as u32);
        }
        let mut c = (*dir).children;
        while !c.is_null() {
            let len = (*c).name.iter().position(|&b| b == 0).unwrap_or(64);
            let name = core::str::from_utf8(&(*c).name[..len]).unwrap_or("?");
            let kind = match (*c).vtype {
                VnodeType::Dir => "<DIR>",
                VnodeType::Chr => "<chr>",
                VnodeType::Blk => "<blk>",
                VnodeType::Reg => "",
            };
            let _ = writeln!(out, "{:<14} {}", name, kind);
            c = (*c).next;
        }
    }
    out
}

pub fn register_chardev(name: &str, dev: DevOps) -> *mut Vnode {
    let mut d = resolve("/dev");
    if d.is_null() {
        d = create_node(root(), "dev", VnodeType::Dir);
    }
    let node = create_node(d, name, VnodeType::Chr);
    if !node.is_null() {
        unsafe { (*node).dev = dev };
    }
    kprintln!("[VFS] Registered /dev/{}", name);
    node
}

pub fn register_blockdev(name: &str, dev: DevOps) -> *mut Vnode {
    let mut d = resolve("/dev");
    if d.is_null() {
        d = create_node(root(), "dev", VnodeType::Dir);
    }
    let node = create_node(d, name, VnodeType::Blk);
    if !node.is_null() {
        unsafe { (*node).dev = dev };
    }
    kprintln!("[VFS] Registered /dev/{} (block)", name);
    node
}

// --------------------------- device dispatch ---------------------------

fn dev_read(node: *mut Vnode, offset: i64, buf: &mut [u8]) -> i32 {
    let dev = unsafe { (*node).dev };
    match dev {
        DevOps::Console => {
            for b in buf.iter_mut() {
                *b = uart::getc();
            }
            buf.len() as i32
        }
        DevOps::Null | DevOps::Vcons => 0,
        DevOps::Zero => {
            for b in buf.iter_mut() {
                *b = 0;
            }
            buf.len() as i32
        }
        DevOps::Rng => virtio::rng::read(buf) as i32,
        DevOps::Blk => {
            if offset as usize % SECTOR != 0 || buf.len() % SECTOR != 0 {
                return -1;
            }
            let sectors = buf.len() / SECTOR;
            let sector = offset as u64 / SECTOR as u64;
            for i in 0..sectors {
                if !virtio::blk::read(sector + i as u64, &mut buf[i * SECTOR..(i + 1) * SECTOR]) {
                    return -1;
                }
            }
            buf.len() as i32
        }
        DevOps::Fat32File => crate::fs::fat32::read_file(node, offset as u64, buf),
        DevOps::Proc(kind) => {
            let content = crate::fs::proc::generate(kind);
            let bytes = content.as_bytes();
            let off = offset as usize;
            if off >= bytes.len() {
                return 0;
            }
            let n = core::cmp::min(buf.len(), bytes.len() - off);
            buf[..n].copy_from_slice(&bytes[off..off + n]);
            n as i32
        }
        DevOps::None => -1,
    }
}

fn dev_write(node: *mut Vnode, offset: i64, buf: &[u8]) -> i32 {
    let dev = unsafe { (*node).dev };
    match dev {
        DevOps::Console => {
            for &b in buf {
                uart::putc(b);
            }
            buf.len() as i32
        }
        DevOps::Null | DevOps::Zero => buf.len() as i32,
        DevOps::Vcons => {
            let n = virtio::console::send(buf);
            if n < 0 {
                -1
            } else {
                n
            }
        }
        DevOps::Blk => {
            if offset as usize % SECTOR != 0 || buf.len() % SECTOR != 0 {
                return -1;
            }
            let sectors = buf.len() / SECTOR;
            let sector = offset as u64 / SECTOR as u64;
            for i in 0..sectors {
                if !virtio::blk::write(sector + i as u64, &buf[i * SECTOR..(i + 1) * SECTOR]) {
                    return -1;
                }
            }
            buf.len() as i32
        }
        DevOps::Rng | DevOps::Fat32File | DevOps::Proc(_) | DevOps::None => -1,
    }
}

// --------------------------- fd table ---------------------------

#[repr(C)]
struct File {
    vnode: *mut Vnode,
    offset: i64,
}

#[repr(C)]
pub struct FdTable {
    fds: [*mut File; MAX_FDS],
}

/// Create an fd table with stdin/stdout/stderr (0/1/2) -> /dev/console.
pub fn fd_table_create() -> *mut FdTable {
    let t = kmalloc(core::mem::size_of::<FdTable>()) as *mut FdTable;
    if t.is_null() {
        return t;
    }
    unsafe {
        core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<FdTable>());
    }
    let console = resolve("/dev/console");
    if !console.is_null() {
        for fd in 0..3 {
            let f = kmalloc(core::mem::size_of::<File>()) as *mut File;
            if !f.is_null() {
                unsafe {
                    (*f).vnode = console;
                    (*f).offset = 0;
                    (*t).fds[fd] = f;
                }
            }
        }
    }
    t
}

pub fn fd_table_destroy(t: *mut FdTable) {
    if t.is_null() {
        return;
    }
    unsafe {
        for i in 0..MAX_FDS {
            if !(*t).fds[i].is_null() {
                kfree((*t).fds[i] as usize);
            }
        }
        kfree(t as usize);
    }
}

fn alloc_fd(t: *mut FdTable) -> i32 {
    unsafe {
        for i in 0..MAX_FDS {
            if (*t).fds[i].is_null() {
                return i as i32;
            }
        }
    }
    -1
}

pub fn fd_open(t: *mut FdTable, path: &str) -> i32 {
    let node = resolve(path);
    if node.is_null() {
        return -1;
    }
    let fd = alloc_fd(t);
    if fd < 0 {
        return -1;
    }
    let f = kmalloc(core::mem::size_of::<File>()) as *mut File;
    if f.is_null() {
        return -1;
    }
    unsafe {
        (*f).vnode = node;
        (*f).offset = 0;
        (*t).fds[fd as usize] = f;
    }
    fd
}

pub fn fd_read(t: *mut FdTable, fd: i32, buf: &mut [u8]) -> i32 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let r = dev_read((*f).vnode, (*f).offset, buf);
        if r > 0 && (*(*f).vnode).vtype != VnodeType::Chr {
            (*f).offset += r as i64;
        }
        r
    }
}

pub fn fd_write(t: *mut FdTable, fd: i32, buf: &[u8]) -> i32 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let r = dev_write((*f).vnode, (*f).offset, buf);
        if r > 0 && (*(*f).vnode).vtype != VnodeType::Chr {
            (*f).offset += r as i64;
        }
        r
    }
}

pub fn fd_close(t: *mut FdTable, fd: i32) -> i32 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        if (*t).fds[fd as usize].is_null() {
            return -1;
        }
        kfree((*t).fds[fd as usize] as usize);
        (*t).fds[fd as usize] = core::ptr::null_mut();
    }
    0
}

pub fn fd_seek(t: *mut FdTable, fd: i32, offset: i64, whence: i64) -> i64 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let vtype = (*(*f).vnode).vtype;
        if vtype == VnodeType::Chr {
            return -1;
        }
        let new_off = match whence {
            SEEK_SET => offset,
            SEEK_CUR => (*f).offset + offset,
            SEEK_END => {
                if vtype != VnodeType::Reg {
                    return -1;
                }
                (*(*f).vnode).size as i64 + offset
            }
            _ => return -1,
        };
        if new_off < 0 {
            return -1;
        }
        (*f).offset = new_off;
        new_off
    }
}
