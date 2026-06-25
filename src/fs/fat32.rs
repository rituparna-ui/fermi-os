//! FAT32 driver (read path) with lazy VFS-backed directory traversal.
//!
//! Port of `src/fs/fat32/fat32.c` + `fat32_vfs.c`. Read-only for now (the
//! original's create/write path can be layered on later). Per-vnode state:
//! `private0` = first cluster, `size` = byte size, `is_dir_fat32` marks a
//! FAT32 directory whose children are resolved on demand.

use super::vfs::{self, DevOps, Vnode, VnodeType};
use crate::kprintln;
use crate::sync::Racy;
use crate::uart;
use crate::virtio::blk;

const SECTOR: usize = 512;
const DIR_ENTRY_SIZE: usize = 32;
const ATTR_LFN: u8 = 0x0F;
const ATTR_VOLUME_ID: u8 = 0x08;
const ATTR_DIRECTORY: u8 = 0x10;
const FAT32_EOC: u32 = 0x0FFF_FFF8;

struct Vol {
    fat_start_sector: u32,
    data_start_sector: u32,
    sectors_per_cluster: u32,
    root_cluster: u32,
    mounted: bool,
}
static VOL: Racy<Vol> = Racy::new(Vol {
    fat_start_sector: 0,
    data_start_sector: 0,
    sectors_per_cluster: 0,
    root_cluster: 0,
    mounted: false,
});

#[repr(C, align(16))]
struct SecBuf([u8; SECTOR]);
static SEC: Racy<SecBuf> = Racy::new(SecBuf([0; SECTOR]));

fn sec() -> &'static mut [u8; SECTOR] {
    unsafe { &mut SEC.get().0 }
}

fn cluster_to_sector(cluster: u32) -> u32 {
    let v = unsafe { VOL.get() };
    v.data_start_sector + (cluster - 2) * v.sectors_per_cluster
}

fn fat_next(cluster: u32) -> u32 {
    let v = unsafe { VOL.get() };
    let fat_offset = cluster * 4;
    let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
    let offset = (fat_offset % SECTOR as u32) as usize;
    if !blk::read(sector as u64, sec()) {
        return FAT32_EOC;
    }
    let b = sec();
    let val = u32::from_le_bytes([b[offset], b[offset + 1], b[offset + 2], b[offset + 3]]);
    val & 0x0FFF_FFFF
}

/// Convert "hello.txt" -> b"HELLO   TXT" (11 bytes, space-padded, uppercase).
fn to_83(name: &[u8]) -> [u8; 11] {
    let mut out = [b' '; 11];
    let up = |c: u8| if c.is_ascii_lowercase() { c - 32 } else { c };
    let mut i = 0;
    let mut o = 0;
    while i < name.len() && o < 8 && name[i] != b'.' {
        out[o] = up(name[i]);
        o += 1;
        i += 1;
    }
    while i < name.len() && name[i] != b'.' {
        i += 1;
    }
    if i < name.len() && name[i] == b'.' {
        i += 1;
    }
    let mut j = 0;
    while i < name.len() && j < 3 {
        out[8 + j] = up(name[i]);
        i += 1;
        j += 1;
    }
    out
}

pub fn mount() {
    let v = unsafe { VOL.get() };
    if !blk::read(0, sec()) {
        uart::errorln("[FS][FAT32] Failed to read BPB");
        return;
    }
    let b = sec();
    let bytes_per_sector = u16::from_le_bytes([b[11], b[12]]);
    let sectors_per_cluster = b[13];
    let reserved_sectors = u16::from_le_bytes([b[14], b[15]]);
    let num_fats = b[16];
    let root_entries_16 = u16::from_le_bytes([b[17], b[18]]);
    let fat_size_16 = u16::from_le_bytes([b[22], b[23]]);
    let fat_size_32 = u32::from_le_bytes([b[36], b[37], b[38], b[39]]);
    let root_cluster = u32::from_le_bytes([b[44], b[45], b[46], b[47]]);

    if bytes_per_sector as usize != SECTOR {
        uart::errorln("[FS][FAT32] Unsupported sector size");
        return;
    }
    if fat_size_16 != 0 || root_entries_16 != 0 {
        uart::errorln("[FS][FAT32] Not a FAT32 volume");
        return;
    }
    v.sectors_per_cluster = sectors_per_cluster as u32;
    v.fat_start_sector = reserved_sectors as u32;
    v.data_start_sector = reserved_sectors as u32 + num_fats as u32 * fat_size_32;
    v.root_cluster = root_cluster;
    v.mounted = true;
    kprintln!(
        "[FS][FAT32] Mounted: sec/clus={} FAT@{} data@{} root={}",
        v.sectors_per_cluster, v.fat_start_sector, v.data_start_sector, v.root_cluster
    );
}

/// Scan a directory cluster chain for an 8.3 name. Returns (cluster,size,attr).
fn dir_lookup(dir_cluster: u32, target: &[u8; 11]) -> Option<(u32, u32, u8)> {
    let v = unsafe { VOL.get() };
    let mut cluster = dir_cluster;
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(cluster);
        for s in 0..v.sectors_per_cluster {
            if !blk::read((base + s) as u64, sec()) {
                return None;
            }
            let b = sec();
            let mut off = 0;
            while off < SECTOR {
                let e = &b[off..off + DIR_ENTRY_SIZE];
                if e[0] == 0x00 {
                    return None; // end of directory
                }
                if e[0] == 0xE5 || e[11] == ATTR_LFN || e[11] & ATTR_VOLUME_ID != 0 {
                    off += DIR_ENTRY_SIZE;
                    continue;
                }
                if e[..11] == target[..] {
                    let hi = u16::from_le_bytes([e[20], e[21]]) as u32;
                    let lo = u16::from_le_bytes([e[26], e[27]]) as u32;
                    let size = u32::from_le_bytes([e[28], e[29], e[30], e[31]]);
                    return Some(((hi << 16) | lo, size, e[11]));
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(cluster);
    }
    None
}

/// Read up to `buf.len()` bytes of a file starting at `first_cluster`.
fn read_chain(first_cluster: u32, size: u32, buf: &mut [u8]) -> i32 {
    let v = unsafe { VOL.get() };
    let mut remaining = core::cmp::min(size as usize, buf.len());
    let total = remaining;
    let mut cluster = first_cluster;
    let mut out = 0usize;
    while remaining > 0 && cluster < FAT32_EOC {
        let base = cluster_to_sector(cluster);
        for s in 0..v.sectors_per_cluster {
            if remaining == 0 {
                break;
            }
            if !blk::read((base + s) as u64, sec()) {
                return -1;
            }
            let chunk = core::cmp::min(remaining, SECTOR);
            buf[out..out + chunk].copy_from_slice(&sec()[..chunk]);
            out += chunk;
            remaining -= chunk;
        }
        cluster = fat_next(cluster);
    }
    (total - remaining) as i32
}

/// VFS lazy lookup: resolve `name` inside FAT32 directory vnode `dir`,
/// creating a child vnode on demand.
pub fn lookup(dir: *mut Vnode, name: &[u8]) -> *mut Vnode {
    if name.len() > 12 {
        return core::ptr::null_mut();
    }
    let dir_cluster = unsafe { (*dir).private0 } as u32;
    let target = to_83(name);
    let (cluster, size, attr) = match dir_lookup(dir_cluster, &target) {
        Some(x) => x,
        None => return core::ptr::null_mut(),
    };
    let is_dir = attr & ATTR_DIRECTORY != 0;
    // Build a NUL-free name string for the vnode.
    let mut namebuf = [0u8; 13];
    let n = core::cmp::min(name.len(), 12);
    namebuf[..n].copy_from_slice(&name[..n]);
    let name_str = core::str::from_utf8(&namebuf[..n]).unwrap_or("?");
    let vtype = if is_dir { VnodeType::Dir } else { VnodeType::Reg };
    let child = vfs::create_node(dir, name_str, vtype);
    if child.is_null() {
        return child;
    }
    unsafe {
        (*child).private0 = cluster as u64;
        (*child).size = size as u64;
        if is_dir {
            (*child).is_dir_fat32 = true;
        } else {
            (*child).dev = DevOps::Fat32File;
        }
    }
    child
}

/// Read from a FAT32-backed regular file vnode at byte `offset`.
pub fn read_file(node: *mut Vnode, offset: u64, buf: &mut [u8]) -> i32 {
    let (first_cluster, size) = unsafe { ((*node).private0 as u32, (*node).size) };
    if offset >= size {
        return 0;
    }
    let remaining = size - offset;
    let to_read = core::cmp::min(remaining as usize, buf.len());
    let total_needed = offset as usize + to_read;
    // Read [0, offset+to_read) into a scratch buffer, copy the requested slice.
    let mut tmp = alloc::vec![0u8; total_needed];
    let got = read_chain(first_cluster, total_needed as u32, &mut tmp);
    if got < 0 {
        return -1;
    }
    let got = got as usize;
    if got <= offset as usize {
        return 0;
    }
    let avail = core::cmp::min(to_read, got - offset as usize);
    buf[..avail].copy_from_slice(&tmp[offset as usize..offset as usize + avail]);
    avail as i32
}

/// Write a 32-bit FAT entry (preserving the top 4 reserved bits).
fn fat_write(cluster: u32, value: u32) -> bool {
    let v = unsafe { VOL.get() };
    let fat_offset = cluster * 4;
    let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
    let offset = (fat_offset % SECTOR as u32) as usize;
    if !blk::read(sector as u64, sec()) {
        return false;
    }
    let b = sec();
    let existing = u32::from_le_bytes([b[offset], b[offset + 1], b[offset + 2], b[offset + 3]]);
    let val = (existing & 0xF000_0000) | (value & 0x0FFF_FFFF);
    b[offset..offset + 4].copy_from_slice(&val.to_le_bytes());
    blk::write(sector as u64, sec())
}

/// Scan the FAT for a free cluster, mark it end-of-chain, return it (0 = fail).
fn fat_alloc_cluster() -> u32 {
    let v = unsafe { VOL.get() };
    let mut c = 2u32;
    loop {
        let fat_offset = c * 4;
        let sector = v.fat_start_sector + fat_offset / SECTOR as u32;
        if sector >= v.data_start_sector {
            return 0; // past end of FAT
        }
        let offset = (fat_offset % SECTOR as u32) as usize;
        if !blk::read(sector as u64, sec()) {
            return 0;
        }
        let b = sec();
        let val = u32::from_le_bytes([b[offset], b[offset + 1], b[offset + 2], b[offset + 3]]) & 0x0FFF_FFFF;
        if val == 0 {
            if !fat_write(c, 0x0FFF_FFFF) {
                return 0;
            }
            return c;
        }
        c += 1;
    }
}

/// Find a free directory slot and write `entry` (32 bytes) into it.
fn dir_add_entry(dir_cluster: u32, entry: &[u8; 32]) -> bool {
    let v = unsafe { VOL.get() };
    let mut cluster = dir_cluster;
    while cluster < FAT32_EOC {
        let base = cluster_to_sector(cluster);
        for srel in 0..v.sectors_per_cluster {
            if !blk::read((base + srel) as u64, sec()) {
                return false;
            }
            let mut off = 0;
            while off < SECTOR {
                let first = sec()[off];
                if first == 0x00 || first == 0xE5 {
                    sec()[off..off + 32].copy_from_slice(entry);
                    return blk::write((base + srel) as u64, sec());
                }
                off += DIR_ENTRY_SIZE;
            }
        }
        cluster = fat_next(cluster);
    }
    false
}

/// Write file data along a cluster chain, one sector at a time.
fn cluster_write_data(first_cluster: u32, data: &[u8]) -> bool {
    let v = unsafe { VOL.get() };
    let mut remaining = data.len();
    let mut src = 0usize;
    let mut cluster = first_cluster;
    while remaining > 0 && cluster < FAT32_EOC {
        let base = cluster_to_sector(cluster);
        for srel in 0..v.sectors_per_cluster {
            if remaining == 0 {
                break;
            }
            let chunk = core::cmp::min(remaining, SECTOR);
            let buf = sec();
            for x in buf.iter_mut() {
                *x = 0;
            }
            buf[..chunk].copy_from_slice(&data[src..src + chunk]);
            if !blk::write((base + srel) as u64, sec()) {
                return false;
            }
            src += chunk;
            remaining -= chunk;
        }
        cluster = fat_next(cluster);
    }
    true
}

/// Create a root-level file `name` (8.3) with `data`. Returns success.
pub fn create(name: &[u8], data: &[u8]) -> bool {
    let v = unsafe { VOL.get() };
    if !v.mounted {
        return false;
    }
    let bytes_per_cluster = v.sectors_per_cluster * SECTOR as u32;
    let clusters_needed = if data.is_empty() {
        0
    } else {
        (data.len() as u32 + bytes_per_cluster - 1) / bytes_per_cluster
    };
    let mut first_cluster = 0u32;
    let mut prev = 0u32;
    for i in 0..clusters_needed {
        let c = fat_alloc_cluster();
        if c == 0 {
            return false;
        }
        if i == 0 {
            first_cluster = c;
        }
        if prev != 0 && !fat_write(prev, c) {
            return false;
        }
        prev = c;
    }
    if !data.is_empty() && first_cluster != 0 && !cluster_write_data(first_cluster, data) {
        return false;
    }
    // Build the 32-byte directory entry.
    let mut e = [0u8; 32];
    e[..11].copy_from_slice(&to_83(name));
    e[11] = 0x20; // ATTR_ARCHIVE
    e[20..22].copy_from_slice(&((first_cluster >> 16) as u16).to_le_bytes());
    e[26..28].copy_from_slice(&(first_cluster as u16).to_le_bytes());
    e[28..32].copy_from_slice(&(data.len() as u32).to_le_bytes());
    dir_add_entry(v.root_cluster, &e)
}

/// Attach the mounted FAT32 root to an existing empty VFS directory.
pub fn vfs_mount(path: &str) {
    let v = unsafe { VOL.get() };
    if !v.mounted {
        uart::errorln("[FAT32] not mounted; call mount() first");
        return;
    }
    let mp = vfs::resolve(path);
    if mp.is_null() {
        kprintln!("[FAT32] mount point {} missing", path);
        return;
    }
    unsafe {
        if (*mp).vtype != VnodeType::Dir {
            kprintln!("[FAT32] mount point {} not a directory", path);
            return;
        }
        (*mp).is_dir_fat32 = true;
        (*mp).private0 = v.root_cluster as u64;
    }
    kprintln!("[FAT32] Mounted at {} (root cluster {})", path, v.root_cluster);
}
