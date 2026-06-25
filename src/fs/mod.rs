//! Filesystem layer: the VFS vnode tree, device nodes, and concrete
//! filesystems (FAT32, /proc) that plug in via the VFS vtables.

#![allow(dead_code)]

pub mod devices;
pub mod proc;
pub mod vfs;
