//! FAT32 filesystem, backed by virtio-blk and plugged into the VFS via lazy
//! directory lookup. Parses the BPB, walks FAT cluster chains, resolves 8.3
//! names, reads files, and (for completeness) creates files. Each VFS vnode
//! carries `(first_cluster, size)` private state allocated on demand.

use crate::drivers::virtio::blk;
use crate::fs::vfs::{self, File, FileOperations, Vnode, VnodeOperations, VnodeType};
use crate::klib::sync::SpinLock;
use crate::kprintln;
use crate::mm::heap;

const SECTOR: usize = 512;
const DIR_ENTRY_SIZE: usize = 32;
const ATTR_LFN: u8 = 0x0F;
const ATTR_VOLUME_ID: u8 = 0x08;
const ATTR_DIRECTORY: u8 = 0x10;
const FAT32_EOC: u32 = 0x0FFF_FFF8;

#[derive(Clone, Copy, Default)]
struct Volume {
    fat_start_sector: u32,
    data_start_sector: u32,
    sectors_per_cluster: u32,
    root_cluster: u32,
    bytes_per_cluster: u32,
    mounted: bool,
}

static VOL: SpinLock<Volume> = SpinLock::new(Volume {
    fat_start_sector: 0,
    data_start_sector: 0,
    sectors_per_cluster: 0,
    root_cluster: 0,
    bytes_per_cluster: 0,
    mounted: false,
});

fn vol() -> Volume {
    *VOL.lock()
}

fn cluster_to_sector(v: &Volume, cluster: u32) -> u32 {
    v.data_start_sector + (cluster - 2) * v.sectors_per_cluster
}

/// Read the next cluster in the chain from the FAT.
fn fat_next(v: &Volume, cluster: u32) -> u32 {
    let fat_offset = cluster * 4;
    let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
    let offset = (fat_offset % SECTOR as u32) as usize;
    let mut buf = [0u8; SECTOR];
    if !blk::read(sector as u64, &mut buf) {
        return FAT32_EOC;
    }
    let val = u32::from_le_bytes([
        buf[offset],
        buf[offset + 1],
        buf[offset + 2],
        buf[offset + 3],
    ]);
    val & 0x0FFF_FFFF
}

/// Convert "hello.txt" -> "HELLO   TXT" (11 bytes, space-padded, uppercase).
fn to_83(name: &[u8]) -> [u8; 11] {
    let mut out = [b' '; 11];
    let mut i = 0;
    // base name (<= 8)
    let mut k = 0;
    while k < name.len() && name[k] != b'.' && i < 8 {
        let mut c = name[k];
        if c.is_ascii_lowercase() {
            c -= 32;
        }
        out[i] = c;
        i += 1;
        k += 1;
    }
    // skip to extension
    while k < name.len() && name[k] != b'.' {
        k += 1;
    }
    if k < name.len() && name[k] == b'.' {
        k += 1;
    }
    let mut j = 0;
    while k < name.len() && j < 3 {
        let mut c = name[k];
        if c.is_ascii_lowercase() {
            c -= 32;
        }
        out[8 + j] = c;
        j += 1;
        k += 1;
    }
    out
}

/// Mount the FAT32 volume from sector 0's BPB. Returns true on success.
pub fn mount() -> bool {
    let mut buf = [0u8; SECTOR];
    if !blk::read(0, &mut buf) {
        crate::klib::uart::Uart.errorln("[FS][FAT32] Failed to read BPB");
        return false;
    }

    let bytes_per_sector = u16::from_le_bytes([buf[11], buf[12]]);
    let sectors_per_cluster = buf[13] as u32;
    let reserved_sectors = u16::from_le_bytes([buf[14], buf[15]]) as u32;
    let num_fats = buf[16] as u32;
    let root_entries_16 = u16::from_le_bytes([buf[17], buf[18]]);
    let fat_size_16 = u16::from_le_bytes([buf[22], buf[23]]);
    let fat_size_32 = u32::from_le_bytes([buf[36], buf[37], buf[38], buf[39]]);
    let root_cluster = u32::from_le_bytes([buf[44], buf[45], buf[46], buf[47]]);

    if bytes_per_sector as usize != SECTOR {
        crate::klib::uart::Uart.errorln("[FS][FAT32] Unsupported sector size");
        return false;
    }
    if fat_size_16 != 0 || root_entries_16 != 0 {
        crate::klib::uart::Uart.errorln("[FS][FAT32] Not a FAT32 volume");
        return false;
    }

    let mut v = VOL.lock();
    v.sectors_per_cluster = sectors_per_cluster;
    v.fat_start_sector = reserved_sectors;
    v.data_start_sector = reserved_sectors + num_fats * fat_size_32;
    v.root_cluster = root_cluster;
    v.bytes_per_cluster = sectors_per_cluster * SECTOR as u32;
    v.mounted = true;
    let snap = *v;
    drop(v);

    kprintln!(
        "[FS][FAT32] Mounted: Sectors/Cluster={} FAT Start@{} data@{} root={}",
        snap.sectors_per_cluster,
        snap.fat_start_sector,
        snap.data_start_sector,
        snap.root_cluster
    );
    true
}

/// Search `dir_cluster` for an 8.3 `target`. Returns (cluster, size, attr).
fn dir_lookup(v: &Volume, dir_cluster: u32, target: &[u8; 11]) -> Option<(u32, u32, u8)> {
    let mut cluster = dir_cluster;
    let mut buf = [0u8; SECTOR];
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(v, cluster);
        for s in 0..v.sectors_per_cluster {
            if !blk::read((base + s) as u64, &mut buf) {
                return None;
            }
            let mut off = 0;
            while off < SECTOR {
                let e = &buf[off..off + DIR_ENTRY_SIZE];
                if e[0] == 0x00 {
                    return None; // end of directory
                }
                if e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME_ID) != 0 {
                    off += DIR_ENTRY_SIZE;
                    continue;
                }
                if e[0..11] == target[..] {
                    let first_hi = u16::from_le_bytes([e[20], e[21]]) as u32;
                    let first_lo = u16::from_le_bytes([e[26], e[27]]) as u32;
                    let size = u32::from_le_bytes([e[28], e[29], e[30], e[31]]);
                    return Some(((first_hi << 16) | first_lo, size, e[11]));
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(v, cluster);
    }
    None
}

/// Look up a single name inside a directory. Returns (first_cluster, size, is_dir).
pub fn lookup_in_dir(dir_cluster: u32, name: &[u8]) -> Option<(u32, u32, bool)> {
    let v = vol();
    if !v.mounted {
        return None;
    }
    let target = to_83(name);
    dir_lookup(&v, dir_cluster, &target).map(|(c, s, attr)| (c, s, attr & ATTR_DIRECTORY != 0))
}

/// Return the raw 11-byte 8.3 name of the `index`-th real entry in
/// `dir_cluster` (skipping free / LFN / volume-id slots), or None past the end.
fn dir_nth_name(v: &Volume, dir_cluster: u32, index: usize) -> Option<[u8; 11]> {
    let mut cluster = dir_cluster;
    let mut buf = [0u8; SECTOR];
    let mut seen = 0usize;
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(v, cluster);
        for s in 0..v.sectors_per_cluster {
            if !blk::read((base + s) as u64, &mut buf) {
                return None;
            }
            let mut off = 0;
            while off < SECTOR {
                let e = &buf[off..off + DIR_ENTRY_SIZE];
                if e[0] == 0x00 {
                    return None; // end of directory
                }
                if e[0] != 0xE5 && e[11] != ATTR_LFN && (e[11] & ATTR_VOLUME_ID) == 0 {
                    if seen == index {
                        let mut name = [0u8; 11];
                        name.copy_from_slice(&e[0..11]);
                        return Some(name);
                    }
                    seen += 1;
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(v, cluster);
    }
    None
}

/// Format an 8.3 padded name ("HELLO   TXT") into "HELLO.TXT" in `out`.
/// Returns bytes written.
fn fmt_83(raw: &[u8; 11], out: &mut [u8]) -> usize {
    let mut p = 0;
    // base (trim trailing spaces)
    let base_len = (0..8).rev().find(|&i| raw[i] != b' ').map_or(0, |i| i + 1);
    for i in 0..base_len {
        if p < out.len() {
            out[p] = raw[i];
            p += 1;
        }
    }
    // extension
    let ext_len = (0..3).rev().find(|&i| raw[8 + i] != b' ').map_or(0, |i| i + 1);
    if ext_len > 0 && p < out.len() {
        out[p] = b'.';
        p += 1;
        for i in 0..ext_len {
            if p < out.len() {
                out[p] = raw[8 + i];
                p += 1;
            }
        }
    }
    p
}

/// Read `size` bytes of a cluster chain starting at `first_cluster` into `buf`.
/// Returns bytes read, or -1 on error.
pub fn read_file(first_cluster: u32, size: u32, buf: &mut [u8]) -> i64 {
    let v = vol();
    if !v.mounted {
        return -1;
    }
    let size = core::cmp::min(size as usize, buf.len()) as u32;
    let mut remaining = size;
    let mut cluster = first_cluster;
    let mut out_pos = 0usize;
    let mut sec = [0u8; SECTOR];

    while remaining > 0 && cluster < FAT32_EOC {
        let base = cluster_to_sector(&v, cluster);
        let mut s = 0;
        while s < v.sectors_per_cluster && remaining > 0 {
            if !blk::read((base + s) as u64, &mut sec) {
                return -1;
            }
            let chunk = core::cmp::min(remaining as usize, SECTOR);
            buf[out_pos..out_pos + chunk].copy_from_slice(&sec[..chunk]);
            out_pos += chunk;
            remaining -= chunk as u32;
            s += 1;
        }
        cluster = fat_next(&v, cluster);
    }
    (size - remaining) as i64
}

/// Write a 32-bit FAT entry for `cluster`, preserving the top 4 reserved bits.
fn fat_write(v: &Volume, cluster: u32, value: u32) -> bool {
    let fat_offset = cluster * 4;
    let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
    let offset = (fat_offset % SECTOR as u32) as usize;
    let mut buf = [0u8; SECTOR];
    if !blk::read(sector as u64, &mut buf) {
        return false;
    }
    let existing = u32::from_le_bytes([
        buf[offset],
        buf[offset + 1],
        buf[offset + 2],
        buf[offset + 3],
    ]);
    let v32 = (existing & 0xF000_0000) | (value & 0x0FFF_FFFF);
    buf[offset..offset + 4].copy_from_slice(&v32.to_le_bytes());
    blk::write(sector as u64, &buf)
}

/// Scan the FAT for a free cluster (entry == 0), mark it end-of-chain, return
/// its number (0 on failure).
fn fat_alloc_cluster(v: &Volume) -> u32 {
    let mut buf = [0u8; SECTOR];
    let mut c = 2u32; // clusters 0 and 1 are reserved
    loop {
        let fat_offset = c * 4;
        let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
        if sector >= v.data_start_sector {
            return 0; // past end of FAT
        }
        let offset = (fat_offset % SECTOR as u32) as usize;
        if !blk::read(sector as u64, &mut buf) {
            return 0;
        }
        let val = u32::from_le_bytes([
            buf[offset],
            buf[offset + 1],
            buf[offset + 2],
            buf[offset + 3],
        ]) & 0x0FFF_FFFF;
        if val == 0 {
            if !fat_write(v, c, 0x0FFF_FFFF) {
                return 0;
            }
            return c;
        }
        c += 1;
    }
}

/// Count free clusters (FAT entries == 0) across the FAT. O(volume) — intended
/// for tests / diagnostics, not the hot path.
pub fn count_free_clusters() -> u64 {
    let v = vol();
    if !v.mounted {
        return 0;
    }
    let mut buf = [0u8; SECTOR];
    let mut free = 0u64;
    let mut c = 2u32;
    loop {
        let fat_offset = c * 4;
        let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
        if sector >= v.data_start_sector {
            break;
        }
        let offset = (fat_offset % SECTOR as u32) as usize;
        if !blk::read(sector as u64, &mut buf) {
            break;
        }
        let val = u32::from_le_bytes([buf[offset], buf[offset + 1], buf[offset + 2], buf[offset + 3]])
            & 0x0FFF_FFFF;
        if val == 0 {
            free += 1;
        }
        c += 1;
    }
    free
}

/// Find a free directory entry slot in `dir_cluster` and write `entry` (32 B).
fn dir_add_entry(v: &Volume, dir_cluster: u32, entry: &[u8; DIR_ENTRY_SIZE]) -> bool {
    let mut cluster = dir_cluster;
    let mut buf = [0u8; SECTOR];
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(v, cluster);
        for s in 0..v.sectors_per_cluster {
            if !blk::read((base + s) as u64, &mut buf) {
                return false;
            }
            let mut off = 0;
            while off < SECTOR {
                let first = buf[off];
                if first == 0x00 || first == 0xE5 {
                    buf[off..off + DIR_ENTRY_SIZE].copy_from_slice(entry);
                    return blk::write((base + s) as u64, &buf);
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(v, cluster);
    }
    false
}

/// Free a cluster chain starting at `first_cluster` by zeroing each FAT entry.
/// A first_cluster of 0 (empty file) is a no-op.
fn free_chain(v: &Volume, first_cluster: u32) -> bool {
    let mut cluster = first_cluster;
    while (2..FAT32_EOC).contains(&cluster) {
        let next = fat_next(v, cluster);
        if !fat_write(v, cluster, 0) {
            return false;
        }
        cluster = next;
    }
    true
}

/// Mark the entry named `target` in `dir_cluster` as deleted (name[0] = 0xE5).
fn dir_mark_deleted(v: &Volume, dir_cluster: u32, target: &[u8; 11]) -> bool {
    let mut cluster = dir_cluster;
    let mut buf = [0u8; SECTOR];
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(v, cluster);
        for s in 0..v.sectors_per_cluster {
            if !blk::read((base + s) as u64, &mut buf) {
                return false;
            }
            let mut off = 0;
            while off < SECTOR {
                let e = &buf[off..off + DIR_ENTRY_SIZE];
                if e[0] == 0x00 {
                    return false; // end of directory; not found
                }
                if e[0] != 0xE5 && e[11] != ATTR_LFN && (e[11] & ATTR_VOLUME_ID) == 0
                    && e[0..11] == target[..]
                {
                    buf[off] = 0xE5; // mark free
                    return blk::write((base + s) as u64, &buf);
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(v, cluster);
    }
    false
}

/// Returns true if directory `dir_cluster` has no real entries other than
/// `.` and `..` (used to refuse removing a non-empty directory).
fn dir_is_empty(v: &Volume, dir_cluster: u32) -> bool {
    dir_nth_name(v, dir_cluster, 0)
        .map(|n| {
            // Index 0 is always "."; check there's no third real entry.
            let _ = n;
            dir_nth_name(v, dir_cluster, 2).is_none()
        })
        .unwrap_or(true)
}

/// Remove a file or empty directory at `path`. Frees its cluster chain and
/// marks the directory entry deleted. Refuses to remove a non-empty directory.
/// Returns true on success.
pub fn remove(path: &[u8]) -> bool {
    let v = vol();
    if !v.mounted {
        return false;
    }
    let (dir_cluster, name83) = match resolve_parent(&v, path) {
        Some(p) => p,
        None => return false,
    };
    let (first_cluster, _size, attr) = match dir_lookup(&v, dir_cluster, &name83) {
        Some(e) => e,
        None => return false,
    };

    // Refuse to remove a non-empty directory (only . and .. allowed).
    if attr & ATTR_DIRECTORY != 0 && !dir_is_empty(&v, first_cluster) {
        return false;
    }

    if !free_chain(&v, first_cluster) {
        return false;
    }
    dir_mark_deleted(&v, dir_cluster, &name83)
}

/// Write `data` across a cluster chain, one sector at a time (read-modify-write
/// for the final partial sector).
fn cluster_write_data(v: &Volume, first_cluster: u32, data: &[u8]) -> bool {
    let mut remaining = data.len();
    let mut src = 0usize;
    let mut cluster = first_cluster;
    let mut sec = [0u8; SECTOR];

    while remaining > 0 && cluster < FAT32_EOC {
        let base = cluster_to_sector(v, cluster);
        let mut s = 0;
        while s < v.sectors_per_cluster && remaining > 0 {
            let chunk = core::cmp::min(remaining, SECTOR);
            if chunk < SECTOR {
                for b in sec.iter_mut() {
                    *b = 0;
                }
            }
            sec[..chunk].copy_from_slice(&data[src..src + chunk]);
            if !blk::write((base + s) as u64, &sec) {
                return false;
            }
            src += chunk;
            remaining -= chunk;
            s += 1;
        }
        cluster = fat_next(v, cluster);
    }
    true
}

/// Split `path` into (parent dir cluster, final 8.3 name). Walks all leading
/// path components as directories starting from the root, returning None if any
/// component is missing or not a directory. A leading '/' is tolerated.
fn resolve_parent(v: &Volume, path: &[u8]) -> Option<(u32, [u8; 11])> {
    let mut i = 0;
    while i < path.len() && path[i] == b'/' {
        i += 1;
    }
    let mut dir_cluster = v.root_cluster;
    loop {
        // Extract the next component [i, j).
        let start = i;
        while i < path.len() && path[i] != b'/' {
            i += 1;
        }
        let comp = &path[start..i];
        if comp.is_empty() {
            return None; // trailing slash / empty component
        }
        // Skip the separator(s).
        while i < path.len() && path[i] == b'/' {
            i += 1;
        }
        if i >= path.len() {
            // Last component — this is the name to create in dir_cluster.
            return Some((dir_cluster, to_83(comp)));
        }
        // Intermediate component: must be an existing directory; descend.
        let (c, _sz, attr) = dir_lookup(v, dir_cluster, &to_83(comp))?;
        if attr & ATTR_DIRECTORY == 0 {
            return None;
        }
        dir_cluster = c;
    }
}

/// Build a directory entry into `de`. `attr` is ATTR_ARCHIVE for files /
/// ATTR_DIRECTORY for dirs.
fn build_dir_entry(name83: &[u8; 11], attr: u8, first_cluster: u32, size: u32) -> [u8; DIR_ENTRY_SIZE] {
    let mut de = [0u8; DIR_ENTRY_SIZE];
    de[..11].copy_from_slice(name83);
    de[11] = attr;
    de[20..22].copy_from_slice(&((first_cluster >> 16) as u16).to_le_bytes()); // hi
    de[26..28].copy_from_slice(&(first_cluster as u16).to_le_bytes()); // lo
    de[28..32].copy_from_slice(&size.to_le_bytes());
    de
}

/// Create a file at `path` with `data` (supports subdirectory paths, e.g.
/// "SUBDIR/FILE.TXT"). Allocates a cluster chain, writes the data, and adds a
/// directory entry in the parent. Refuses duplicates. Returns true on success.
pub fn create(path: &[u8], data: &[u8]) -> bool {
    let v = vol();
    if !v.mounted {
        return false;
    }
    let (dir_cluster, name83) = match resolve_parent(&v, path) {
        Some(p) => p,
        None => return false,
    };
    let len = data.len() as u32;

    // Refuse duplicates: no overwrite, no duplicate dir entry.
    if dir_lookup(&v, dir_cluster, &name83).is_some() {
        return false;
    }

    // Allocate + chain clusters for the data.
    let clusters_needed = if len == 0 {
        0
    } else {
        (len + v.bytes_per_cluster - 1) / v.bytes_per_cluster
    };
    let mut first_cluster = 0u32;
    let mut prev_cluster = 0u32;
    for i in 0..clusters_needed {
        let c = fat_alloc_cluster(&v);
        if c == 0 {
            return false;
        }
        if i == 0 {
            first_cluster = c;
        }
        if prev_cluster != 0 && !fat_write(&v, prev_cluster, c) {
            return false;
        }
        prev_cluster = c;
    }

    if len > 0 && first_cluster != 0 && !cluster_write_data(&v, first_cluster, data) {
        return false;
    }

    let de = build_dir_entry(&name83, 0x20 /* ATTR_ARCHIVE */, first_cluster, len);
    dir_add_entry(&v, dir_cluster, &de)
}

/// Create a directory at `path` (supports nested paths whose parent exists).
/// Allocates one cluster, zero-initializes it, writes the `.` and `..` entries,
/// and adds a directory entry in the parent. Refuses duplicates.
pub fn mkdir(path: &[u8]) -> bool {
    let v = vol();
    if !v.mounted {
        return false;
    }
    let (parent_cluster, name83) = match resolve_parent(&v, path) {
        Some(p) => p,
        None => return false,
    };
    if dir_lookup(&v, parent_cluster, &name83).is_some() {
        return false;
    }

    // Allocate the new directory's first cluster and zero every sector.
    let dir_cluster = fat_alloc_cluster(&v);
    if dir_cluster == 0 {
        return false;
    }
    let base = cluster_to_sector(&v, dir_cluster);
    let zero = [0u8; SECTOR];
    for s in 0..v.sectors_per_cluster {
        if !blk::write((base + s) as u64, &zero) {
            return false;
        }
    }

    // Write the "." (self) and ".." (parent; 0 means root, per FAT convention)
    // entries into the first sector.
    let mut sec = [0u8; SECTOR];
    let dot = build_dir_entry(b".          ", ATTR_DIRECTORY, dir_cluster, 0);
    let parent_link = if parent_cluster == v.root_cluster { 0 } else { parent_cluster };
    let dotdot = build_dir_entry(b"..         ", ATTR_DIRECTORY, parent_link, 0);
    sec[0..DIR_ENTRY_SIZE].copy_from_slice(&dot);
    sec[DIR_ENTRY_SIZE..2 * DIR_ENTRY_SIZE].copy_from_slice(&dotdot);
    if !blk::write(base as u64, &sec) {
        return false;
    }

    let de = build_dir_entry(&name83, ATTR_DIRECTORY, dir_cluster, 0);
    dir_add_entry(&v, parent_cluster, &de)
}

pub fn root_cluster() -> u32 {
    vol().root_cluster
}

/// True if `path` resolves to an existing on-disk entry (bypasses the VFS
/// cache — reads the directory fresh). Useful for verifying create/remove.
pub fn exists(path: &[u8]) -> bool {
    let v = vol();
    if !v.mounted {
        return false;
    }
    match resolve_parent(&v, path) {
        Some((dir, name83)) => dir_lookup(&v, dir, &name83).is_some(),
        None => false,
    }
}

/// Read a file's contents by path, bypassing the VFS vnode cache (resolve the
/// parent + dir entry fresh from disk, then read the chain). Returns bytes read
/// or -1. Useful for verifying create/remove churn where the cache would hold
/// stale vnodes for a repeatedly recreated name.
pub fn read_path(path: &[u8], buf: &mut [u8]) -> i64 {
    let v = vol();
    if !v.mounted {
        return -1;
    }
    let (dir, name83) = match resolve_parent(&v, path) {
        Some(p) => p,
        None => return -1,
    };
    match dir_lookup(&v, dir, &name83) {
        Some((first_cluster, size, _attr)) => read_file(first_cluster, size, buf),
        None => -1,
    }
}

// --- VFS integration --------------------------------------------------------

/// Per-vnode FAT32 state (heap-allocated, pointed to by Vnode.private_data).
#[repr(C)]
struct Fat32Priv {
    first_cluster: u32,
    size: u32,
}

static FILE_OPS: FileOperations = FileOperations {
    read: Some(fat32_file_read),
    write: None,
};
static DIR_OPS: VnodeOperations = VnodeOperations {
    lookup: Some(fat32_lookup),
    readdir: Some(fat32_readdir),
};

fn fat32_readdir(dir: *mut Vnode, index: usize, name_out: *mut u8, cap: usize) -> i64 {
    // SAFETY: dir is a live FAT32-backed directory vnode.
    let pd = unsafe { (*dir).private_data as *const Fat32Priv };
    if pd.is_null() {
        return -1;
    }
    let v = vol();
    if !v.mounted {
        return -1;
    }
    let first_cluster = unsafe { (*pd).first_cluster };
    match dir_nth_name(&v, first_cluster, index) {
        Some(raw) => {
            let mut tmp = [0u8; 13];
            let n = fmt_83(&raw, &mut tmp);
            let copy = core::cmp::min(n, cap);
            unsafe { core::ptr::copy_nonoverlapping(tmp.as_ptr(), name_out, copy) };
            copy as i64
        }
        None => -1,
    }
}

fn fat32_lookup(dir: *mut Vnode, name: *const u8, namelen: usize) -> *mut Vnode {
    unsafe {
        let pd = (*dir).private_data as *const Fat32Priv;
        if pd.is_null() || namelen > 12 {
            return core::ptr::null_mut();
        }
        let name_slice = core::slice::from_raw_parts(name, namelen);
        let (cluster, size, is_dir) = match lookup_in_dir((*pd).first_cluster, name_slice) {
            Some(t) => t,
            None => return core::ptr::null_mut(),
        };

        // Allocate child private state before creating the vnode (so a failure
        // doesn't leave a dangling node in the directory cache).
        let cpd = heap::kmalloc(core::mem::size_of::<Fat32Priv>()) as *mut Fat32Priv;
        if cpd.is_null() {
            return core::ptr::null_mut();
        }
        (*cpd).first_cluster = cluster;
        (*cpd).size = size;

        let name_str = core::str::from_utf8(name_slice).unwrap_or("?");
        let vtype = if is_dir { VnodeType::Dir } else { VnodeType::Reg };
        let child = vfs::create_node(dir, name_str, vtype);
        if child.is_null() {
            heap::kfree(cpd as *mut u8);
            return core::ptr::null_mut();
        }
        (*child).private_data = cpd as *mut ();
        (*child).size = size as u64;
        if is_dir {
            (*child).v_ops = &DIR_OPS;
        } else {
            (*child).ops = &FILE_OPS;
        }
        child
    }
}

fn fat32_file_read(n: *mut Vnode, f: *mut File, buf: *mut u8, count: usize) -> i64 {
    unsafe {
        let pd = (*n).private_data as *const Fat32Priv;
        if pd.is_null() {
            return -1;
        }
        let size = (*pd).size;
        let offset = (*f).offset as u32;
        if offset >= size {
            return 0; // EOF
        }
        let remaining = (size - offset) as usize;
        let to_read = core::cmp::min(remaining, count) as u32;

        // read_file reads from cluster start; to support an arbitrary offset we
        // read the [0, offset+to_read) prefix into a scratch buffer and slice.
        let total_needed = offset + to_read;
        let tmp = heap::kmalloc(total_needed as usize);
        if tmp.is_null() {
            return -1;
        }
        let tmp_slice = core::slice::from_raw_parts_mut(tmp, total_needed as usize);
        let got = read_file((*pd).first_cluster, total_needed, tmp_slice);
        if got < 0 {
            heap::kfree(tmp);
            return -1;
        }
        let got = got as u32;
        if got <= offset {
            heap::kfree(tmp);
            return 0;
        }
        let actual = if got < total_needed {
            got - offset
        } else {
            to_read
        };
        core::ptr::copy_nonoverlapping(tmp.add(offset as usize), buf, actual as usize);
        heap::kfree(tmp);
        (*f).offset += actual as i64;
        actual as i64
    }
}

/// Mount the FAT32 root at an existing VFS directory `path`.
pub fn vfs_mount(path: &str) -> bool {
    let mp = vfs::resolve(path);
    if mp.is_null() {
        kprintln!("[FAT32] Mount point {} does not exist", path);
        return false;
    }
    unsafe {
        if (*mp).vtype != VnodeType::Dir {
            kprintln!("[FAT32] Mount point {} is not a directory", path);
            return false;
        }
        let pd = heap::kmalloc(core::mem::size_of::<Fat32Priv>()) as *mut Fat32Priv;
        if pd.is_null() {
            return false;
        }
        (*pd).first_cluster = root_cluster();
        (*pd).size = 0;
        (*mp).private_data = pd as *mut ();
        (*mp).v_ops = &DIR_OPS;
        kprintln!("[FAT32] Mounted at {} (root cluster {})", path, (*pd).first_cluster);
    }
    true
}
