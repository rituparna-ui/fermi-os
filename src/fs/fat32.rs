//! FAT32 driver (VFS-backed). Stub — full implementation lands next.

use super::vfs::Vnode;

pub fn mount() {
    // Implemented in the FAT32 milestone.
}

/// Lazy directory lookup. Returns null until FAT32 is implemented.
pub fn lookup(_dir: *mut Vnode, _name: &[u8]) -> *mut Vnode {
    core::ptr::null_mut()
}

/// Read from a FAT32-backed regular file.
pub fn read_file(_node: *mut Vnode, _offset: u64, _buf: &mut [u8]) -> i32 {
    -1
}
