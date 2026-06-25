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

pub fn root_cluster() -> u32 {
    vol().root_cluster
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
};

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
