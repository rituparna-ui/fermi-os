# FAT32 Filesystem Driver & VFS Integration - Porting Spec

## Overview

The FAT32 subsystem provides a complete FAT32 filesystem driver with lazy directory traversal and cluster-chain walking. It integrates with the VFS layer for file operations (read/write) and directory traversal via vnode lookup. The driver:

- Mounts a FAT32 volume from block device (VirtIO block)
- Parses the Boot Parameter Block (BPB) and validates FAT32 format
- Implements file lookup by path (supporting subdirectories like "DOCS/README.TXT")
- Provides cluster-chain reading for multi-cluster files
- Implements file creation with cluster allocation and FAT chain updates
- Integrates with VFS via per-vnode private state (fat32_priv) for lazy traversal
- Implements file read with offset support (currently O(n) per call due to no seek-at support)

**Key Characteristics:**
- Single mounted volume (static global `vol` state)
- Per-vnode lazy lookup (children only instantiated on demand)
- Sector-level I/O via block layer (512-byte sectors, 16-byte aligned buffers)
- No write support yet (file creation works, but file modify/append not implemented)
- All 8.3 name conversions done in-memory (no LFN support)
- Preserves upper 4 bits of FAT entries (reserved for future use)

---

## Public API

### Header: `fat32.h`

#### Initialization & Mount

```c
int fat32_mount(void)
```
- **Behavior:** Reads and parses BPB from sector 0. Validates FAT32 format (rejects FAT12/16). Initializes global `vol` state (fat_start_sector, data_start_sector, sectors_per_cluster, root_cluster, bytes_per_cluster).
- **Returns:** `ESUCCESS` (1) on success, `EERROR` (0) on parse error or unsupported format.
- **Errors:** Rejects if sector size != 512, if FAT12/16 indicators present (fat_size_16 != 0 or root_entries_16 != 0), or if block I/O fails.
- **Side Effects:** Initializes static `vol` struct. Logs via `uart_printf()`.

#### File Lookup

```c
int fat32_find(const char *path, uint32_t *out_first_cluster, uint32_t *out_size)
```
- **Behavior:** Performs path traversal from root cluster. Parses path by splitting on '/' into 8.3 components. For each component, looks up in current directory using `dir_lookup()`. Intermediate components must be directories. Handles leading slashes. Supports arbitrary depth (e.g., "DOCS/SUB/FILE.TXT").
- **Parameters:**
  - `path`: null-terminated string (can start with '/').
  - `out_first_cluster`: receives starting cluster number of final entry (32-bit, hi/lo merged).
  - `out_size`: receives file size in bytes (only meaningful for files, 0 for directories).
- **Returns:** `ESUCCESS` (1) if found, `EERROR` (0) if not found or intermediate path is not a directory.
- **Behavior Under Subdirs:** If intermediate component is not a directory, returns error immediately.

#### File Read

```c
int fat32_read(uint32_t first_cluster, uint32_t size, void *buf, uint32_t buf_len)
```
- **Behavior:** Reads up to `min(size, buf_len)` bytes from file cluster chain. Follows FAT chain starting at `first_cluster`. Reads entire 512-byte sectors into static sector buffer, copies requested portion to output buffer.
- **Parameters:**
  - `first_cluster`: starting cluster (must be >= 2 and < FAT32_EOC).
  - `size`: number of bytes to read (from file size).
  - `buf`: output buffer.
  - `buf_len`: output buffer capacity.
- **Returns:** Number of bytes actually read (can be less than requested if file is shorter), or -1 on I/O error.
- **Cluster Chain:** Stops when FAT entry >= FAT32_EOC (0x0FFFFFF8 or higher).

#### File Creation

```c
int fat32_create(const char *path, const void *data, uint32_t len)
```
- **Behavior:** Creates a new file with given data. Parses path to extract parent directory and filename. Allocates cluster chain via `fat_alloc_cluster()`. Links clusters via FAT updates. Writes data via `cluster_write_data()`. Adds directory entry to parent directory.
- **Parameters:**
  - `path`: destination path (supports subdirectories, e.g., "DOCS/NEWFILE.TXT").
  - `data`: pointer to file contents (can be NULL if len==0).
  - `len`: file size in bytes.
- **Returns:** `ESUCCESS` (1) on success, `EERROR` (0) on cluster allocation failure, I/O error, or parent directory not found.
- **Side Effects:** Modifies FAT and data sectors. Adds entry to parent directory.
- **Empty Files:** Supports creating 0-byte files (no clusters allocated).

#### Directory Lookup (VFS Helper)

```c
int fat32_lookup_in_dir(uint32_t dir_cluster, const char *name,
                        uint32_t *out_first_cluster, uint32_t *out_size,
                        int *out_is_dir)
```
- **Behavior:** Single-level directory lookup (no path traversal). Searches directory at `dir_cluster` for 8.3 name match. Returns cluster, size, and type information. Used by VFS layer for lazy traversal.
- **Parameters:**
  - `dir_cluster`: directory's starting cluster.
  - `name`: null-terminated 8.3 name component (or shorter).
  - `out_first_cluster`: receives cluster number.
  - `out_size`: receives size (0 for directories).
  - `out_is_dir`: receives 1 if directory, 0 if file.
- **Returns:** `ESUCCESS` (1) if found, `EERROR` (0) if not found.

#### VFS Root Cluster

```c
uint32_t fat32_root_cluster(void)
```
- **Behavior:** Returns the root cluster number from the mounted volume's BPB. Used during VFS mount to initialize root vnode private data.
- **Returns:** Root cluster number (typically 2 on FAT32).

#### VFS Mount

```c
int fat32_vfs_mount(const char *path)
```
- **Behavior:** Attaches FAT32 filesystem to VFS at given mount point. Mount point must exist as an empty directory vnode. Allocates fat32_priv for mount point, sets vnode->v_ops to fat32_dir_ops. Subsequent lookups through that vnode will use FAT32 traversal.
- **Parameters:**
  - `path`: VFS path to mount point (e.g., "/mnt/fat32").
- **Returns:** `ESUCCESS` (1) on successful mount, `EERROR` (0) if mount point doesn't exist or is not a directory.
- **Side Effects:** Registers directory operations on mount point vnode. Allocates heap memory for private data.

---

## VFS Integration (`fat32_vfs.c`)

### Per-Vnode Private State

```c
typedef struct fat32_priv {
  uint32_t first_cluster;  /* cluster where file/dir starts */
  uint32_t size;           /* bytes (0 for directories) */
} fat32_priv_t;
```

### Vnode Operations

#### Directory Lookup (VFS Entry Point)

```c
static vnode_t *fat32_lookup(vnode_t *dir, const char *name, size_t namelen)
```
- **Behavior:** VFS vnode_operations.lookup callback. Converts name to null-terminated C string. Calls `fat32_lookup_in_dir()` to find entry in directory at `dir->private_data->first_cluster`. On success, allocates fat32_priv for child, creates child vnode via `vfs_create_node()`, attaches private_data and appropriate v_ops/ops.
- **Allocation Order:** Allocates fat32_priv BEFORE calling vfs_create_node() to avoid dangling pointers if vfs_create_node succeeds but subsequent operations fail.
- **Returns:** Pointer to newly created child vnode, or NULL if not found / allocation failed.
- **Type Detection:** Sets vnode->v_ops = fat32_dir_ops if is_dir, else vnode->ops = fat32_file_ops.

#### File Read (VFS Entry Point)

```c
static int fat32_file_read(vnode_t *n, file_t *f, void *buf, size_t count)
```
- **Behavior:** VFS file_operations.read callback. Implements file read with offset support. Checks f->offset against file size. Reads [0, offset+to_read) range into temporary malloc'd buffer. Copies [offset, offset+to_read) slice to output buf. Updates f->offset.
- **Performance Note:** O(n) per call (reads entire prefix). Future optimization: implement fat32_read_at(cluster, offset_in_file, len, buf) for O(1) seeks.
- **Parameters:**
  - `n`: vnode (file).
  - `f`: file struct with current offset.
  - `buf`: output buffer.
  - `count`: number of bytes to read.
- **Returns:** Number of bytes read, 0 on EOF, -1 on error.
- **Memory:** Allocates temporary buffer via kmalloc (freed after copy).

---

## Data Structures

### Boot Parameter Block (BPB)

```c
struct bpb {
  uint8_t jmp[3];               /* JMP instruction (offset 0x00) */
  uint8_t oem[8];               /* OEM identifier (offset 0x03) */
  uint16_t bytes_per_sector;    /* Typically 512 (offset 0x0B) */
  uint8_t sectors_per_cluster;  /* Power of 2, typically 8 (offset 0x0D) */
  uint16_t reserved_sectors;    /* Sectors before FAT (offset 0x0E) */
  uint8_t num_fats;             /* Number of FAT copies (offset 0x10) */
  uint16_t root_entries_16;     /* Must be 0 on FAT32 (offset 0x11) */
  uint16_t total_sectors_16;    /* Must be 0 on FAT32 (offset 0x13) */
  uint8_t media;                /* Media descriptor (offset 0x15) */
  uint16_t fat_size_16;         /* Must be 0 on FAT32 (offset 0x16) */
  uint16_t sectors_per_track;   /* CHS geometry (offset 0x18) */
  uint16_t num_heads;           /* CHS geometry (offset 0x1A) */
  uint32_t hidden_sectors;      /* Hidden sectors (offset 0x1C) */
  uint32_t total_sectors_32;    /* Total sectors for large volumes (offset 0x20) */
  /* FAT32-specific fields (offset 0x24 onward) */
  uint32_t fat_size_32;         /* FAT size in sectors (offset 0x24) */
  uint16_t ext_flags;           /* Extension flags (offset 0x28) */
  uint16_t fs_version;          /* Filesystem version (offset 0x2A) */
  uint32_t root_cluster;        /* Root directory cluster (offset 0x2C), typically 2 */
} __attribute__((packed));
```

**Magic Numbers (FAT32 Validation):**
- `fat_size_16 == 0`: Must be 0 on FAT32 (distinguishes from FAT12/16).
- `root_entries_16 == 0`: Must be 0 on FAT32.
- `bytes_per_sector == 512`: Currently only 512-byte sectors supported.

**Offsets (from sector 0):**
- BPB fields are at fixed offsets within the 512-byte boot sector.
- Memcpy into struct automatically handles little-endian ARM aarch64.

### Directory Entry

```c
struct dir_entry {
  uint8_t name[11];              /* 8.3 padded with 0x20 spaces (offset 0x00) */
  uint8_t attr;                  /* Attribute bits (offset 0x0B) */
  uint8_t nt_res;                /* NT reserved byte (offset 0x0C) */
  uint8_t ctime_tenth;           /* Creation time tenths (offset 0x0D) */
  uint16_t ctime;                /* Creation time (offset 0x0E) */
  uint16_t cdate;                /* Creation date (offset 0x10) */
  uint16_t adate;                /* Last access date (offset 0x12) */
  uint16_t first_cluster_hi;     /* High word of cluster (offset 0x14) */
  uint16_t wtime;                /* Write time (offset 0x16) */
  uint16_t wdate;                /* Write date (offset 0x18) */
  uint16_t first_cluster_lo;     /* Low word of cluster (offset 0x1A) */
  uint32_t size;                 /* File size in bytes (offset 0x1C) */
} __attribute__((packed));
```

**Size:** 32 bytes per entry (DIR_ENTRY_SIZE = 32).

**Attribute Bits:**
- `ATTR_LFN = 0x0F`: Long filename entry (skipped).
- `ATTR_VOLUME_ID = 0x08`: Volume label (skipped).
- `ATTR_DIRECTORY = 0x10`: This is a directory.
- `ATTR_ARCHIVE = 0x20`: Archive bit (set by fat32_create).

**Special Values:**
- `name[0] == 0x00`: End of directory marker (no more entries).
- `name[0] == 0xE5`: Deleted entry (skip in traversal).

**Cluster Encoding:**
- Full 32-bit cluster = (first_cluster_hi << 16) | first_cluster_lo.
- Clusters 0-1 are reserved; data clusters start at 2.

---

## Filesystem Constants

```c
#define SECTOR                512         /* Sector size in bytes */
#define DIR_ENTRY_SIZE         32         /* 32-byte directory entries */
#define ATTR_LFN            0x0F         /* Long filename marker */
#define ATTR_VOLUME_ID      0x08         /* Volume ID marker */
#define ATTR_DIRECTORY      0x10         /* Directory flag */
#define FAT32_EOC        0x0FFFFFF8      /* End-of-cluster marker (any value >= this) */
```

---

## Internal Helpers (Static Functions)

### Cluster Geometry

```c
static uint32_t cluster_to_sector(uint32_t cluster)
```
- **Behavior:** Converts cluster number to starting sector address.
- **Formula:** `vol.data_start_sector + (cluster - 2) * vol.sectors_per_cluster`
- **Note:** Clusters 0-1 are reserved; data clusters start at index 2.

### FAT Navigation

```c
static uint32_t fat_next(uint32_t cluster)
```
- **Behavior:** Reads FAT entry for given cluster. Returns next cluster in chain, or FAT32_EOC if end-of-chain.
- **FAT Structure:** FAT entries are 32-bit values. Offset in FAT = cluster * 4 bytes.
- **Entry Format:** Lower 28 bits are cluster number; upper 4 bits are reserved (masked out: `val & 0x0FFFFFFF`).
- **Returns:** 28-bit cluster number or FAT32_EOC.

```c
static int fat_write(uint32_t cluster, uint32_t value)
```
- **Behavior:** Writes FAT entry for given cluster. Preserves upper 4 reserved bits of existing entry. Used during cluster allocation.
- **Parameters:**
  - `cluster`: cluster number to update.
  - `value`: new value (28-bit cluster number, typically FAT32_EOC for end-of-chain).
- **Returns:** `ESUCCESS` on success, `EERROR` on I/O error.

### Directory Traversal

```c
static int dir_lookup(uint32_t dir_cluster, const uint8_t target[11],
                      uint32_t *out_cluster, uint32_t *out_size,
                      uint8_t *out_attr)
```
- **Behavior:** Searches a directory for a matching 8.3 name. Walks cluster chain of directory. For each cluster, reads each sector sequentially. Compares 11-byte name fields against target (exact match, case-sensitive). Skips LFN entries (attr == 0x0F), volume ID (attr & 0x08), deleted entries (name[0] == 0xE5), and stops at end marker (name[0] == 0x00).
- **Returns:** `ESUCCESS` if match found (fills output pointers), `EERROR` if not found or I/O error.

### Name Conversion

```c
static void to_83(const char *name, uint8_t out[11])
```
- **Behavior:** Converts null-terminated C string to 8.3 format (11 bytes, space-padded, uppercase). Splits on '.' to separate name and extension. Left-pads name to 8 bytes, extension to 3 bytes, with 0x20 (space) characters. Converts lowercase to uppercase.
- **Example:** "hello.txt" → "HELLO   TXT" (5 + 3 spaces + 3 bytes).
- **Edge Cases:** No '.' → name only, 3 spaces for extension. Truncates name to 8, extension to 3. Handles leading '/' (skipped elsewhere in path parsing).

### Cluster Allocation

```c
static uint32_t fat_alloc_cluster(void)
```
- **Behavior:** Scans FAT from cluster 2 onwards until finding an entry == 0 (free). Marks it as FAT32_EOC (0x0FFFFFF8). Returns allocated cluster number.
- **Returns:** Allocated cluster number (> 0), or 0 on failure (no free clusters, I/O error, or past end of FAT).
- **Bound:** Stops at vol.data_start_sector (assumes no free clusters beyond FAT extent).

### Directory Entry Addition

```c
static int dir_add_entry(uint32_t dir_cluster, const struct dir_entry *entry)
```
- **Behavior:** Finds free slot in directory (name[0] == 0x00 or 0xE5). Memcpys entry into slot. Writes sector back to block device.
- **Returns:** `ESUCCESS` on success, `EERROR` if directory full or I/O error.

### Cluster Chain Write

```c
static int cluster_write_data(uint32_t first_cluster, const void *data,
                              uint32_t len)
```
- **Behavior:** Writes data across cluster chain. Iterates through clusters following FAT chain. For each sector, writes up to 512 bytes. If final partial sector, zero-fills remainder. Stops when remaining == 0 or cluster >= FAT32_EOC.
- **Returns:** `ESUCCESS` on completion, `EERROR` on I/O error.

---

## Mounted Volume State (Static Global)

```c
static struct {
  uint32_t fat_start_sector;     /* First sector of FAT table */
  uint32_t data_start_sector;    /* First sector of data region */
  uint32_t sectors_per_cluster;  /* Typically 8 */
  uint32_t root_cluster;         /* Root directory cluster (typically 2) */
  uint32_t bytes_per_cluster;    /* sectors_per_cluster * 512 */
} vol;
```

**Initialization:** Filled by `fat32_mount()` from BPB. Used by all subsequent operations.

**Calculations:**
- `fat_start_sector = reserved_sectors` (from BPB).
- `data_start_sector = reserved_sectors + (num_fats * fat_size_32)`.
- `bytes_per_cluster = sectors_per_cluster * 512`.

---

## I/O Buffering

```c
static uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
```

**Purpose:** Single 512-byte sector-aligned buffer used for all I/O. Shared across read/write/lookup operations. Sector alignment (16-byte) required by block layer.

**Lifecycle:** Data persists only until next I/O operation. No caching between calls.

---

## Block Layer Interface

Used from `blk/blk.h`:

```c
int blk_read(uint64_t sector, void *buf);
int blk_write(uint64_t sector, const void *buf);
```

- **Parameters:**
  - `sector`: LBA (logical block address, 512-byte sectors).
  - `buf`: must be 16-byte aligned, 512 bytes.
- **Returns:** `ESUCCESS` (1) on success, `EERROR` (0) on I/O error.

---

## Boot/Usage Ordering

1. **Early Kernel Initialization** (`kernel.c`):
   - Initialize PCI, VirtIO block device.
   - Call `fat32_mount()` to parse BPB and initialize vol.
   - Call `vfs_init()` to set up VFS root.

2. **Mount Point Setup**:
   - Create `/mnt` and `/mnt/fat32` directories via `vfs_create_node()`.
   - Call `fat32_vfs_mount("/mnt/fat32")` to attach FAT32 at mount point.

3. **File Access**:
   - User processes open files via syscall `sys_open("/mnt/fat32/HELLO.TXT")`.
   - VFS resolves path, performs lazy vnode lookups at each directory level.
   - fat32_lookup() called for each directory component.
   - Final vnode's file_operations.read() called for file I/O.

---

## Rust Module Design

### Architecture

**Modules:**
- `fat32::core`: Raw FAT32 structures, BPB parsing, cluster math.
- `fat32::fat`: FAT table reading/writing, cluster allocation.
- `fat32::dir`: Directory traversal, name conversion, entry lookup.
- `fat32::file`: File read/write, cluster chain walking.
- `fat32::vfs_ops`: VFS integration layer, lazy lookup, file operations.
- `fat32::mount`: Mounted volume state, initialization.

### Types

```rust
// Core structures (packed, repr(C))
pub struct Bpb { ... }           // Boot Parameter Block
pub struct DirEntry { ... }      // 32-byte directory entry
pub struct Fat32Priv { ... }     // Per-vnode private state (fat32_priv_t)

// Module state
pub struct MountedVolume {       // vol global state
  pub fat_start_sector: u32,
  pub data_start_sector: u32,
  pub sectors_per_cluster: u32,
  pub root_cluster: u32,
  pub bytes_per_cluster: u32,
}

// Result type
pub enum Error {
  IoBad,
  NotFound,
  NotDirectory,
  NotFat32,
  AllocationFailed,
}
type Result<T> = std::result::Result<T, Error>;
```

### Statics & Locking

```rust
// Single mounted volume, protected by Mutex for interior mutability
lazy_static! {
  static ref VOLUME: Mutex<Option<MountedVolume>> = Mutex::new(None);
}

// Sector-aligned buffer (single, no concurrent access needed due to sync I/O model)
static SECTOR_BUF: [u8; 512] = [0u8; 512];
```

**Rationale:** FAT32 mounts are single per kernel. Block I/O is synchronous. Sector buffer is shared but only used within single-threaded I/O operations.

### Key Functions

```rust
pub fn mount() -> Result<()> {
  // Read BPB, validate FAT32, initialize VOLUME
}

pub fn find(path: &str) -> Result<(u32, u32)> {
  // Path traversal with subdirectory support
  // Returns (first_cluster, size)
}

pub fn read(first_cluster: u32, size: u32, buf: &mut [u8]) -> Result<usize> {
  // Cluster chain reading, return bytes read
}

pub fn create(path: &str, data: &[u8]) -> Result<()> {
  // File creation with cluster allocation
}

pub fn lookup_in_dir(dir_cluster: u32, name: &str) -> Result<(u32, u32, bool)> {
  // Single-level lookup for VFS
  // Returns (cluster, size, is_dir)
}

pub fn root_cluster() -> u32 {
  // Return root cluster
}

pub fn vfs_mount(path: &str) -> Result<()> {
  // Attach FAT32 to VFS
}
```

### VFS Integration Layer

```rust
pub struct Fat32DirOps;
impl VnodeOps for Fat32DirOps {
  fn lookup(&self, vnode: &Vnode, name: &str) -> Option<Vnode> {
    // Call lookup_in_dir(), create child vnode with fat32_priv
  }
}

pub struct Fat32FileOps;
impl FileOps for Fat32FileOps {
  fn read(&self, vnode: &Vnode, file: &mut File, buf: &mut [u8]) -> Result<usize> {
    // Fetch from fat32_priv, call read() with offset support
  }
}
```

### Safety Considerations

1. **Memory Alignment:** SECTOR_BUF declared with `#[repr(align(16))]` for block layer requirements.
2. **Packed Structures:** BPB and DirEntry use `#[repr(C, packed)]` to match exact C layout.
3. **Bounds Checking:** All array accesses guarded (e.g., cluster validation, sector offsets).
4. **No Unsafe:** Minimize unsafe blocks; only for packed struct field access if needed.
5. **Error Handling:** Result-based error propagation (no panics in production code).

---

## Implementation Gotchas

1. **Cluster Math:** Clusters are 0-indexed in formulas but 0-1 are reserved. Sector address = data_start + (cluster - 2) * spc. Off-by-one errors common.

2. **FAT Entry Masking:** FAT entries are 32-bit with upper 4 bits reserved. Must mask: `val & 0x0FFFFFFF`. Failing to mask breaks cluster chain detection.

3. **Directory Chains:** Root directory on FAT32 is a cluster chain (not a fixed area). Must follow FAT entries like regular clusters.

4. **8.3 Name Conversion:** Space-padding (not null-termination) for shorter names. "README" → "README  TXT" (3 trailing spaces). Off by one on padding common.

5. **Sector Alignment:** Block layer requires 16-byte aligned buffers. Stack allocation fails; must use static or heap with explicit alignment.

6. **Empty Files:** Supported (len == 0, no clusters allocated). First cluster can be 0; don't dereference before checking size.

7. **Deleted Entries:** Name[0] == 0xE5 marks deleted entries. Must skip; don't treat as end-of-directory (0x00 is end).

8. **LFN Entries:** Attr == 0x0F indicates long filename entry (not classic 8.3). Skip in flat 8.3 implementation.

9. **Volume ID:** Attr & 0x08 indicates volume label. Skip; don't confuse with regular files.

10. **Offset Reads (VFS):** Current fat32_file_read() reads entire [0, offset+len) prefix O(n) per call. Future: implement fat32_read_at(cluster, byte_offset, len, buf) for O(1) seeks.

11. **FAT Write Preservation:** When writing FAT entry, preserve upper 4 bits: `(existing & 0xF0000000) | (value & 0x0FFFFFFF)`. Forgetting this corrupts reserved flags.

12. **End-of-Cluster Marker:** FAT32_EOC = 0x0FFFFFF8; any value >= this is end-of-chain. Some implementations use 0x0FFFFFFF; both valid per spec.

13. **Partial Sectors:** When writing data smaller than sector size, zero-fill remainder (sec_buf set to 0 before partial write). Prevents garbage in uninitialized sectors.

14. **Directory Entry Allocation:** Find free slot (name[0] == 0x00 or 0xE5) before adding new entry. No bounds check for full directory; will scan to FAT region if not careful. In production, add capacity limit.

15. **Multi-FAT:** BPB specifies num_fats (typically 2). Current implementation reads/writes only first FAT. Mirroring to backup FAT not implemented.

---

## Hardware Details & Magic Constants

**BPB Offsets (from sector 0):**
- 0x00-0x02: Jump instruction (JMP)
- 0x03-0x0A: OEM name
- 0x0B-0x0C: Bytes per sector (u16, LE) — must be 512
- 0x0D: Sectors per cluster (u8)
- 0x0E-0x0F: Reserved sectors (u16, LE)
- 0x10: Number of FATs (u8)
- 0x11-0x12: Root entries (u16, LE) — must be 0 for FAT32
- 0x13-0x14: Total sectors 16-bit (u16, LE) — must be 0 for FAT32
- 0x15: Media descriptor (u8)
- 0x16-0x17: FAT size 16-bit (u16, LE) — must be 0 for FAT32
- 0x18-0x19: Sectors per track (u16, LE) — CHS geometry
- 0x1A-0x1B: Number of heads (u16, LE) — CHS geometry
- 0x1C-0x1F: Hidden sectors (u32, LE)
- 0x20-0x23: Total sectors 32-bit (u32, LE)
- **FAT32-specific (offset 0x24+):**
- 0x24-0x27: FAT size 32-bit (u32, LE)
- 0x28-0x29: Extension flags (u16, LE)
- 0x2A-0x2B: Filesystem version (u16, LE)
- 0x2C-0x2F: Root cluster (u32, LE) — typically 2

**Directory Entry Offsets (32 bytes):**
- 0x00-0x0A: Filename 8.3 (11 bytes, space-padded)
- 0x0B: Attribute byte
- 0x0C: NT reserved
- 0x0D: Creation time tenths
- 0x0E-0x0F: Creation time (u16, LE)
- 0x10-0x11: Creation date (u16, LE)
- 0x12-0x13: Last access date (u16, LE)
- 0x14-0x15: First cluster high (u16, LE)
- 0x16-0x17: Write time (u16, LE)
- 0x18-0x19: Write date (u16, LE)
- 0x1A-0x1B: First cluster low (u16, LE)
- 0x1C-0x1F: File size (u32, LE)

**Block Layer Constants:**
- Sector size: 512 bytes
- Sector alignment: 16 bytes (for MMIO DMA)

**FAT Entry Size:** 4 bytes (32-bit per cluster)

**End-of-Chain Marker:** 0x0FFFFFF8 (or higher, up to 0xFFFFFFFF)

**Reserved Clusters:** 0 and 1 (metadata, not user data)

---

## Subsystem Dependencies

**Depends On:**
- `blk` (block layer read/write)
- `uart` (logging)
- `strings` (memcpy, memset, memcmp)
- `utils` (ESUCCESS/EERROR constants)
- `vfs` (vnode types, operations)
- `heap` (kmalloc, kfree for VFS integration)

**Depended On By:**
- `proc` (file I/O syscalls)
- `vfs` (filesystem driver integration)
- `kernel` (initialization)

---

## Testing & Validation

Key scenarios:
1. Mount valid FAT32 volume (BPB parsing).
2. Find files in root and subdirectories.
3. Read single-cluster and multi-cluster files.
4. Create new files with various sizes (0-byte, partial cluster, multi-cluster).
5. Verify FAT chain correctness (no loops, proper termination).
6. VFS lazy lookup (directory traversal via vnode ops).
7. File read with offset (via VFS file_operations).
8. Edge cases: empty directories, deleted entries, full directories.

