//! Virtual filesystem: a Unix-style vnode tree with per-vnode operation
//! vtables, path resolution, and per-process file-descriptor tables.
//!
//! Mirrors the C design (function-pointer vtables for runtime polymorphism).
//! Vnodes come from a fixed pool; fd tables and `File` objects are heap-
//! allocated. Drivers and filesystems register `FileOperations`/
//! `VnodeOperations` vtables (`&'static`) that the VFS dispatches through.

#![allow(dead_code)]

use crate::kprintln;
use crate::mm::heap;
use core::ptr;

pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;

pub const MAX_FDS: usize = 64;
const MAX_VNODES: usize = 128;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum VnodeType {
    Reg, // regular file
    Dir, // directory
    Chr, // char device
    Blk, // block device
}

/// Per-vnode read/write vtable (the VFS's runtime polymorphism). Each fs/driver
/// fills these in. Functions take the vnode + open file (for offset) + buffer.
#[repr(C)]
pub struct FileOperations {
    pub read: Option<fn(node: *mut Vnode, f: *mut File, buf: *mut u8, count: usize) -> i64>,
    pub write: Option<fn(node: *mut Vnode, f: *mut File, buf: *const u8, count: usize) -> i64>,
}

/// Per-directory tree-traversal vtable: resolve a name on demand (lazy fs), and
/// enumerate entries by index for `readdir`.
#[repr(C)]
pub struct VnodeOperations {
    pub lookup: Option<fn(dir: *mut Vnode, name: *const u8, namelen: usize) -> *mut Vnode>,
    /// Copy the `index`-th entry's name into `name_out` (capacity `cap`).
    /// Returns the name length on success, or -1 when there is no such entry.
    pub readdir:
        Option<fn(dir: *mut Vnode, index: usize, name_out: *mut u8, cap: usize) -> i64>,
}

#[repr(C)]
pub struct Vnode {
    pub name: [u8; 64],
    pub vtype: VnodeType,
    pub ops: *const FileOperations,
    pub v_ops: *const VnodeOperations,
    pub private_data: *mut (),
    pub size: u64,
    pub parent: *mut Vnode,
    pub children: *mut Vnode,
    pub next: *mut Vnode,
}

impl Vnode {
    const fn zeroed() -> Self {
        Self {
            name: [0; 64],
            vtype: VnodeType::Reg,
            ops: ptr::null(),
            v_ops: ptr::null(),
            private_data: ptr::null_mut(),
            size: 0,
            parent: ptr::null_mut(),
            children: ptr::null_mut(),
            next: ptr::null_mut(),
        }
    }
}

/// An open file: a vnode plus a byte offset.
#[repr(C)]
pub struct File {
    pub vnode: *mut Vnode,
    pub offset: i64,
}

/// Per-process file-descriptor table.
#[repr(C)]
pub struct FdTable {
    pub fds: [*mut File; MAX_FDS],
}

// Vnode pool + root. Single-core, set up at boot and mutated during device/fs
// registration (which happens before user tasks run). Held in SyncUnsafeCell
// rather than `static mut` so the "single-core, hand-managed aliasing" intent
// is explicit and references-to-static-mut are avoided.
use crate::klib::sync::SyncUnsafeCell;
static NODE_POOL: SyncUnsafeCell<[Vnode; MAX_VNODES]> =
    SyncUnsafeCell::new([const { Vnode::zeroed() }; MAX_VNODES]);
static NODE_COUNT: SyncUnsafeCell<usize> = SyncUnsafeCell::new(0);
static ROOT: SyncUnsafeCell<*mut Vnode> = SyncUnsafeCell::new(ptr::null_mut());

fn alloc_vnode(name: &[u8], vtype: VnodeType) -> *mut Vnode {
    // SAFETY (single-core): vnode allocation happens during boot/registration;
    // no other context touches the pool concurrently.
    unsafe {
        let count = NODE_COUNT.get();
        if *count >= MAX_VNODES {
            return ptr::null_mut();
        }
        let pool = NODE_POOL.get() as *mut Vnode;
        let n = pool.add(*count);
        *count += 1;
        ptr::write(n, Vnode::zeroed());
        (*n).vtype = vtype;
        let copy = core::cmp::min(name.len(), 63);
        (*n).name[..copy].copy_from_slice(&name[..copy]);
        n
    }
}

/// Initialize the VFS with a root directory.
pub fn init() {
    // SAFETY (single-core): boot-time init.
    unsafe {
        *ROOT.get() = alloc_vnode(b"/", VnodeType::Dir);
    }
    kprintln!("[VFS] Initialized");
}

pub fn root() -> *mut Vnode {
    // SAFETY (single-core): read of a pointer static set at init.
    unsafe { *ROOT.get() }
}

/// Create a node `name` of `vtype` under `parent`, linking it into the tree.
pub fn create_node(parent: *mut Vnode, name: &str, vtype: VnodeType) -> *mut Vnode {
    let n = alloc_vnode(name.as_bytes(), vtype);
    if n.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: n is a fresh pool node; parent (if any) is a live tree node.
    unsafe {
        (*n).parent = parent;
        if !parent.is_null() {
            (*n).next = (*parent).children;
            (*parent).children = n;
        }
    }
    n
}

/// Ensure `/dev` exists, returning it.
fn ensure_dev() -> *mut Vnode {
    let dev = resolve("/dev");
    if dev.is_null() {
        create_node(root(), "dev", VnodeType::Dir)
    } else {
        dev
    }
}

/// Register a char device at `/dev/<name>` with the given ops.
pub fn register_chardev(name: &str, ops: *const FileOperations) -> *mut Vnode {
    let node = create_node(ensure_dev(), name, VnodeType::Chr);
    if !node.is_null() {
        unsafe { (*node).ops = ops };
    }
    kprintln!("[VFS] Registered /dev/{}", name);
    node
}

/// Register a block device at `/dev/<name>` with the given ops.
pub fn register_blockdev(name: &str, ops: *const FileOperations) -> *mut Vnode {
    let node = create_node(ensure_dev(), name, VnodeType::Blk);
    if !node.is_null() {
        unsafe { (*node).ops = ops };
    }
    kprintln!("[VFS] Registered /dev/{} (block)", name);
    node
}

fn name_match(node_name: &[u8; 64], s: &[u8]) -> bool {
    let len = s.len();
    if len >= 64 {
        return false;
    }
    for i in 0..len {
        if node_name[i] != s[i] {
            return false;
        }
    }
    node_name[len] == 0
}

fn find_child(dir: *mut Vnode, name: &[u8]) -> *mut Vnode {
    // SAFETY: dir is a live tree node; children form a valid linked list.
    unsafe {
        let mut c = (*dir).children;
        while !c.is_null() {
            if name_match(&(*c).name, name) {
                return c;
            }
            c = (*c).next;
        }
        // Not cached — ask the filesystem to resolve it lazily.
        if !(*dir).v_ops.is_null() {
            if let Some(lookup) = (*(*dir).v_ops).lookup {
                return lookup(dir, name.as_ptr(), name.len());
            }
        }
    }
    ptr::null_mut()
}

/// Resolve an absolute path to a vnode (handles `.`, `..`, repeated slashes).
pub fn resolve(path: &str) -> *mut Vnode {
    let bytes = path.as_bytes();
    if bytes.is_empty() || bytes[0] != b'/' {
        return ptr::null_mut();
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

        // SAFETY: cur is a live tree node.
        unsafe {
            if (*cur).vtype != VnodeType::Dir {
                return ptr::null_mut();
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
                return ptr::null_mut();
            }
            cur = child;
        }
    }
    cur
}

/// Enumerate the `index`-th entry of directory `dir`, copying its name into
/// `name_out` (capacity `cap`). Returns the name length, or -1 past the end /
/// on a non-directory. Filesystem-backed dirs (FAT32) use their `readdir` vop;
/// in-memory dirs (`/`, `/dev`, `/proc`) fall back to walking the child list.
pub fn readdir(dir: *mut Vnode, index: usize, name_out: *mut u8, cap: usize) -> i64 {
    if dir.is_null() {
        return -1;
    }
    // SAFETY: dir is a live tree node.
    unsafe {
        if (*dir).vtype != VnodeType::Dir {
            return -1;
        }
        if !(*dir).v_ops.is_null() {
            if let Some(rd) = (*(*dir).v_ops).readdir {
                return rd(dir, index, name_out, cap);
            }
        }
        // In-memory fallback: children are a LIFO list, so walk to `index`.
        let mut c = (*dir).children;
        let mut i = 0;
        while !c.is_null() {
            if i == index {
                let nlen = (*c).name.iter().position(|&b| b == 0).unwrap_or(64);
                let n = core::cmp::min(nlen, cap);
                core::ptr::copy_nonoverlapping((*c).name.as_ptr(), name_out, n);
                return n as i64;
            }
            c = (*c).next;
            i += 1;
        }
    }
    -1
}

// --- File-descriptor tables --------------------------------------------------

/// Allocate a zeroed fd table on the heap.
pub fn fd_table_create() -> *mut FdTable {
    let t = heap::kmalloc(core::mem::size_of::<FdTable>()) as *mut FdTable;
    if !t.is_null() {
        unsafe {
            ptr::write(t, FdTable {
                fds: [ptr::null_mut(); MAX_FDS],
            });
        }
    }
    t
}

/// Free an fd table and all its open files.
pub fn fd_table_destroy(t: *mut FdTable) {
    if t.is_null() {
        return;
    }
    unsafe {
        for i in 0..MAX_FDS {
            if !(*t).fds[i].is_null() {
                heap::kfree((*t).fds[i] as *mut u8);
            }
        }
        heap::kfree(t as *mut u8);
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

/// Open `path`, returning a new fd in `t` (or -1).
pub fn fd_open(t: *mut FdTable, path: &str) -> i32 {
    let node = resolve(path);
    if node.is_null() {
        return -1;
    }
    let fd = alloc_fd(t);
    if fd < 0 {
        return -1;
    }
    let f = heap::kmalloc(core::mem::size_of::<File>()) as *mut File;
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

/// Read up to `count` bytes from `fd` into `buf`.
pub fn fd_read(t: *mut FdTable, fd: i32, buf: *mut u8, count: usize) -> i64 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let vnode = (*f).vnode;
        if (*vnode).ops.is_null() {
            return -1;
        }
        match (*(*vnode).ops).read {
            Some(read) => read(vnode, f, buf, count),
            None => -1,
        }
    }
}

/// Write `count` bytes from `buf` to `fd`.
pub fn fd_write(t: *mut FdTable, fd: i32, buf: *const u8, count: usize) -> i64 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let vnode = (*f).vnode;
        if (*vnode).ops.is_null() {
            return -1;
        }
        match (*(*vnode).ops).write {
            Some(write) => write(vnode, f, buf, count),
            None => -1,
        }
    }
}

/// Close `fd`.
pub fn fd_close(t: *mut FdTable, fd: i32) -> i32 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        if (*t).fds[fd as usize].is_null() {
            return -1;
        }
        heap::kfree((*t).fds[fd as usize] as *mut u8);
        (*t).fds[fd as usize] = ptr::null_mut();
    }
    0
}

/// Seek `fd` (SEEK_SET/CUR/END). Char devices are not seekable.
pub fn fd_seek(t: *mut FdTable, fd: i32, offset: i64, whence: i32) -> i64 {
    if fd < 0 || fd as usize >= MAX_FDS {
        return -1;
    }
    unsafe {
        let f = (*t).fds[fd as usize];
        if f.is_null() {
            return -1;
        }
        let vnode = (*f).vnode;
        if (*vnode).vtype == VnodeType::Chr {
            return -1;
        }
        let new_off = match whence {
            SEEK_SET => offset,
            SEEK_CUR => (*f).offset + offset,
            SEEK_END => {
                if (*vnode).vtype != VnodeType::Reg {
                    return -1;
                }
                (*vnode).size as i64 + offset
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
