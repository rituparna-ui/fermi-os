# VFS (Virtual File System) - Porting Specification

## Overview

The VFS subsystem provides a unified abstraction for file I/O across heterogeneous devices (UARTs, RNG, block devices, FAT32 filesystems, /proc pseudo-filesystem). It implements:

1. **Vnode tree**: In-memory filesystem tree (/, /dev, /mnt, /proc) with hierarchical parent-child relationships.
2. **Path resolution**: Recursive descent parsing of absolute paths, supporting `..` and `.` traversal.
3. **Polymorphic I/O**: Dynamic dispatch via vtables (`file_operations_t`, `vnode_operations_t`) to invoke device/filesystem-specific read/write/lookup operations.
4. **File descriptor table**: Per-process fd array (up to 64 fds) mapping to open files with per-file offset tracking.
5. **Syscall integration**: `SYS_OPEN`, `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE` dispatch through VFS to underlying devices/filesystems.

Key insight: VFS is the **polymorphic I/O dispatcher**. Every read(fd, ...) syscall looks up fd → file_t → vnode → vtable → device-specific implementation. The kernel never reads directly from UART or block device; it always routes through VFS.

### Layout

The C implementation lives in:
- `/src/fs/vfs/vfs.h` – public API and type definitions
- `/src/fs/vfs/vfs.c` – vnode tree, path resolution, fd_table management

Related subsystems that depend on VFS:
- **devices.c**: Registers /dev/console, /dev/null, /dev/zero, /dev/rng, /dev/blk, /dev/vcons
- **fat32_vfs.c**: Implements /mnt/fat32 directory traversal and file reading
- **proc.c**: Implements /proc/uptime, /proc/meminfo, /proc/tasks, etc.

Integration points:
- **proc/proc.h**: Each `task_t` holds a `fd_table_t *fds`
- **kernel.c**: Initializes VFS at boot (after MMU, before scheduler)
- **syscall handlers**: Forward read/write/open/close syscalls to VFS

---

## Type Definitions and Constants

### Enumerations and Type Codes

```c
// vnode type identifier
typedef enum {
  VNODE_REG, // Regular file (0)
  VNODE_DIR, // Directory (1)
  VNODE_CHR, // Character device (2)
  VNODE_BLK, // Block device (3)
} vnode_type_t;

// Seek offset anchors
#define SEEK_SET 0   // Absolute offset
#define SEEK_CUR 1   // Relative to current position
#define SEEK_END 2   // Relative to file end
```

### Vtable Structures

```c
/*
 * file_operations_t: Per-vnode read/write dispatch table.
 *
 * Every vnode has an ops pointer that fills this. When a syscall reads from
 * an fd, it extracts f->vnode->ops->read and calls it. Different devices
 * fill in different function pointers:
 *   - UART console: console_read/console_write
 *   - /dev/zero: zero_read (fills buffer with 0x00), zero_write (no-op)
 *   - /dev/null: null_read (EOF), null_write (drop bytes)
 *   - /dev/rng: rng_dev_read (entropy), NULL write
 *   - /dev/blk: blk_dev_read/blk_dev_write (sector-aligned I/O)
 *   - FAT32 files: fat32_file_read, NULL write
 *
 * Signature: read/write receive (vnode, file, buf, count).
 * Return: number of bytes read/written, or -1 on error.
 *
 * Notes:
 *   - read(vnode, file, buf, count): Fills buf[0..count-1] from file->offset.
 *     After a successful read, file->offset is NOT automatically advanced
 *     (caller must use fd_seek if needed, or device handler updates it).
 *     Actually looking at the code: fd_read calls ops->read but does NOT
 *     update f->offset. Only blk_dev_read/blk_dev_write in devices.c
 *     manually advance f->offset. Most devices (console, zero, null, rng)
 *     ignore the offset entirely.
 *   - write(vnode, file, buf, count): Writes buf[0..count-1] to file->offset.
 *     Similarly, offset is NOT auto-advanced. Only blk_dev_write advances it.
 */
typedef struct file_operations {
  int (*read)(struct vnode *node, struct file *f, void *buf, size_t count);
  int (*write)(struct vnode *node, struct file *f, const void *buf,
               size_t count);
} file_operations_t;

/*
 * vnode_operations_t: Per-directory lookup dispatch table.
 *
 * Only directories (VNODE_DIR) have v_ops filled in; it's NULL for files/devices.
 *
 * lookup(dir, name, namelen): Resolve a single path component `name` (len bytes,
 * not NUL-terminated) inside the directory `dir`. Returns the child vnode on
 * success, NULL if not found or error.
 *
 * Used by vfs_resolve during path traversal: for each component, it calls
 * find_child(dir, name, len) which first checks dir->children linked list
 * (the in-memory cache), then falls back to dir->v_ops->lookup if not cached.
 *
 * This allows:
 *   - /dev: lazily scanned from in-memory pool of registered devices
 *   - /mnt/fat32: lazily scanned from FAT32 clusters on disk
 *   - /proc: lazily synthesized from kernel state
 */
typedef struct vnode_operations {
  struct vnode *(*lookup)(struct vnode *dir, const char *name, size_t namelen);
} vnode_operations_t;
```

### Core Structures

```c
/*
 * vnode_t: In-memory vnode (filesystem node).
 *
 * Layout (exact as in struct definition):
 *   char name[64];              // 0x00–0x3f: node name (e.g., "console", "fat32")
 *   vnode_type_t type;          // 0x40: 4-byte enum (VNODE_REG/DIR/CHR/BLK)
 *   file_operations_t *ops;     // 0x48: vtable for read/write (NULL for dirs)
 *   vnode_operations_t *v_ops;  // 0x50: vtable for lookup (NULL for non-dirs)
 *   void *private_data;         // 0x58: fs/driver-specific state (e.g., FAT32 metadata)
 *   uint64_t size;              // 0x60: file size in bytes (0 for dirs/devices)
 *   struct vnode *parent;       // 0x68: parent directory (NULL for root)
 *   struct vnode *children;     // 0x70: linked list head of child nodes
 *   struct vnode *next;         // 0x78: sibling link in parent->children list
 *
 * Memory management:
 *   - Vnodes are allocated from a static pool `node_pool[MAX_VNODES]` (128 nodes max).
 *   - No dynamic allocation of vnode_t itself; node_count tracks next free slot.
 *   - However, private_data (e.g., FAT32 per-file metadata) IS dynamically allocated.
 *   - Device operations (read/write) receive a vnode* and can dereference private_data.
 *
 * Tree structure:
 *   vfs_root() returns the "/" vnode.
 *   Each vnode has parent (points upward) and children (linked list downward).
 *   No explicit "directory" data structure; vnodes ARE the tree nodes.
 *
 * Example tree at boot:
 *   /                         (VNODE_DIR, root, ops=NULL, v_ops=NULL)
 *   ├─ /dev                   (VNODE_DIR, ops=NULL, v_ops=NULL)
 *   │  ├─ /dev/console        (VNODE_CHR, ops=console_ops)
 *   │  ├─ /dev/null           (VNODE_CHR, ops=null_ops)
 *   │  ├─ /dev/zero           (VNODE_CHR, ops=zero_ops)
 *   │  ├─ /dev/rng            (VNODE_CHR, ops=rng_ops)
 *   │  ├─ /dev/blk            (VNODE_BLK, ops=blk_ops)
 *   │  └─ /dev/vcons          (VNODE_CHR, ops=vcons_ops)
 *   ├─ /mnt                   (VNODE_DIR, ops=NULL, v_ops=NULL)
 *   │  └─ /mnt/fat32          (VNODE_DIR, ops=NULL, v_ops=fat32_dir_ops, private_data=fat32_root_cluster)
 *   │     └─ [dynamically scanned from FAT32 clusters]
 *   └─ /proc                  (VNODE_DIR, ops=NULL, v_ops=proc_dir_ops [not shown in code, but implied])
 *      └─ [dynamically synthesized from kernel state]
 */
typedef struct vnode {
  char name[64];
  vnode_type_t type;
  file_operations_t *ops;
  vnode_operations_t *v_ops;
  void *private_data;
  uint64_t size;
  struct vnode *parent;
  struct vnode *children;
  struct vnode *next;
} vnode_t;

/*
 * file_t: Represents an open file with per-fd offset tracking.
 *
 * Layout:
 *   vnode_t *vnode;    // 0x00: pointer to the vnode
 *   int64_t offset;    // 0x08: current file position
 *
 * Created by fd_open:
 *   1. Resolves path → vnode
 *   2. Allocates a fresh file_t from heap
 *   3. Stores in fd_table->fds[fd]
 *
 * Used during read/write:
 *   fd_read/fd_write extract file_t from fd_table, call vnode->ops->read/write.
 *   Most devices ignore offset; only regular files and block devices use it.
 *   fd_seek updates file->offset.
 */
typedef struct file {
  vnode_t *vnode;
  int64_t offset;
} file_t;

/*
 * fd_table_t: Per-process file descriptor table.
 *
 * Layout:
 *   file_t *fds[MAX_FDS];  // 0x00–0x1FF: array of 64 file_t pointers (8 bytes each)
 *
 * Constant:
 *   #define MAX_FDS 64
 *
 * Memory:
 *   - Allocated via kmalloc in fd_table_create.
 *   - Each fds[i] is either NULL (closed) or points to a heap-allocated file_t.
 *   - fd_table_destroy walks the array and kfree each non-NULL entry, then kfree the table.
 *
 * Integration:
 *   - Each task_t has a fd_table_t *fds pointer (see sched.h).
 *   - Syscalls use the current task's fd_table to look up file descriptors.
 */
#define MAX_FDS 64
typedef struct fd_table {
  file_t *fds[MAX_FDS];
} fd_table_t;
```

### Constants and Limits

```c
#define MAX_VNODES 128      // Static pool capacity for vnode_t
#define MAX_FDS 64          // File descriptors per process
#define SEEK_SET 0          // Absolute offset
#define SEEK_CUR 1          // Relative to current position
#define SEEK_END 2          // Relative to file end
```

---

## Public API

### VFS Tree Management

```c
/*
 * void vfs_init(void)
 *
 * Initialize the VFS subsystem. Must be called early at boot (after heap is ready,
 * before any file operations). Creates the root "/" vnode, zeros the vnode pool,
 * and prints a debug message.
 *
 * State changes:
 *   - Allocates root vnode from node_pool[0]
 *   - Sets node_count = 1
 *   - Logs "[VFS] Initialized"
 *
 * Called from: kernel.c early_main after heap_init and before devices_register
 */
void vfs_init(void);

/*
 * vnode_t *vfs_root(void)
 *
 * Return a pointer to the root ("/") vnode. Provides entry point for tree traversal.
 *
 * Returns:
 *   Pointer to static root vnode (never NULL after vfs_init)
 */
vnode_t *vfs_root(void);

/*
 * vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type)
 *
 * Create a new vnode and link it into the tree as a child of `parent`.
 *
 * Parameters:
 *   parent: Parent vnode (or NULL to create an orphan; rarely used)
 *   name: NUL-terminated name (up to 63 chars; truncated if longer)
 *   type: One of VNODE_REG, VNODE_DIR, VNODE_CHR, VNODE_BLK
 *
 * Returns:
 *   Pointer to newly allocated vnode on success
 *   NULL if node_count >= MAX_VNODES (pool exhausted)
 *
 * Side effects:
 *   - Allocates a vnode from node_pool[node_count++]
 *   - Sets vnode.name via strncpy (first 63 chars, NUL-terminated)
 *   - Sets vnode.type
 *   - If parent is non-NULL:
 *       Sets vnode.parent = parent
 *       Prepends vnode onto parent->children linked list
 *   - Zeros all other fields (ops, v_ops, private_data, size, etc.)
 *
 * Usage pattern:
 *   // Create /dev directory
 *   vnode_t *dev = vfs_create_node(vfs_root(), "dev", VNODE_DIR);
 *   // Create /dev/console character device
 *   vnode_t *console = vfs_create_node(dev, "console", VNODE_CHR);
 *   console->ops = &console_ops;  // Manually fill in vtable after creation
 *
 * Calling context: Early boot (from devices_register, fat32_vfs_mount), not from syscalls
 */
vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type);

/*
 * vnode_t *vfs_resolve(const char *path)
 *
 * Recursively resolve an absolute path to a vnode using path traversal.
 * Supports . and .. components.
 *
 * Parameters:
 *   path: Absolute path, NUL-terminated, starting with '/' (e.g., "/dev/console")
 *
 * Returns:
 *   Pointer to resolved vnode on success
 *   NULL if:
 *     - path does not start with '/'
 *     - path is NULL
 *     - any intermediate component is not a directory
 *     - path component not found (not in cache and lookup fails)
 *
 * Algorithm:
 *   1. Validate path starts with '/'
 *   2. cur = root
 *   3. For each path component (delimited by '/'):
 *        - Skip leading slashes
 *        - Extract component name and length
 *        - If component is '.': stay at cur
 *        - If component is '..': cur = cur->parent (if not NULL)
 *        - Else:
 *            child = find_child(cur, component, len)
 *            if child found, cur = child; else return NULL
 *   4. Return cur
 *
 * Lookup strategy (find_child):
 *   1. Linear search cur->children linked list
 *   2. If found in cache, return it
 *   3. Else if cur->v_ops->lookup exists, call it to lazily load
 *   4. Else return NULL
 *
 * This allows lazy loading:
 *   - /dev/console is in cache (registered at boot)
 *   - /mnt/fat32/dir/file is NOT in cache, calls fat32_lookup to pull from disk
 *   - /proc/uptime is NOT in cache, would call proc_lookup (not implemented in this code)
 *
 * Called from: fd_open, vfs_register_chardev, vfs_register_blockdev, recursively
 */
vnode_t *vfs_resolve(const char *path);

/*
 * vnode_t *vfs_register_chardev(const char *name, file_operations_t *ops)
 *
 * Register a character device under /dev/<name>. Creates /dev if missing.
 *
 * Parameters:
 *   name: Device name (e.g., "console", "rng")
 *   ops: Pointer to file_operations_t vtable (read/write function pointers)
 *
 * Returns:
 *   Pointer to created vnode on success
 *   NULL on error (vfs_resolve or vfs_create_node failed)
 *
 * Side effects:
 *   - Resolves "/dev" (creates it if missing)
 *   - Creates a VNODE_CHR child with the given name
 *   - Fills in node->ops = ops
 *   - Prints "[VFS] Registered /dev/<name>"
 *
 * Called from: devices_register at boot for console, null, zero, rng, vcons
 */
vnode_t *vfs_register_chardev(const char *name, file_operations_t *ops);

/*
 * vnode_t *vfs_register_blockdev(const char *name, file_operations_t *ops)
 *
 * Register a block device under /dev/<name>. Creates /dev if missing.
 * Identical to vfs_register_chardev but creates VNODE_BLK instead.
 *
 * Called from: devices_register for /dev/blk
 */
vnode_t *vfs_register_blockdev(const char *name, file_operations_t *ops);
```

### File Descriptor Management

```c
/*
 * fd_table_t *fd_table_create(void)
 *
 * Allocate and initialize an empty file descriptor table.
 *
 * Returns:
 *   Pointer to newly allocated fd_table_t (via kmalloc)
 *   NULL if kmalloc fails
 *
 * Side effects:
 *   - Allocates sizeof(fd_table_t) bytes from heap
 *   - Memsets the allocation to zero (all fds[i] = NULL)
 *
 * Called from: sched_create_task when setting up a new process
 */
fd_table_t *fd_table_create(void);

/*
 * void fd_table_destroy(fd_table_t *t)
 *
 * Free a file descriptor table and all open file_t structures within it.
 *
 * Parameters:
 *   t: fd_table_t pointer (may be NULL, in which case this is a no-op)
 *
 * Side effects:
 *   - Iterates fds[0..MAX_FDS-1]
 *   - For each non-NULL entry, kfree the file_t
 *   - kfree the fd_table_t itself
 *   - Does NOT close vnodes or unmount filesystems
 *
 * Called from: sched_reap when destroying a dead task
 */
void fd_table_destroy(fd_table_t *t);

/*
 * int fd_open(fd_table_t *t, const char *path)
 *
 * Open a file by path, allocate a file descriptor, and return the fd number.
 *
 * Parameters:
 *   t: File descriptor table (from task->fds)
 *   path: Absolute path to open (e.g., "/dev/console")
 *
 * Returns:
 *   File descriptor number (0–63) on success
 *   -1 on error:
 *     - vfs_resolve failed (path not found)
 *     - alloc_fd failed (all 64 fds in use)
 *     - kmalloc failed (insufficient heap)
 *
 * Side effects:
 *   - Resolves path to vnode via vfs_resolve
 *   - Allocates a fresh file_t via kmalloc
 *   - Stores file_t in t->fds[fd]
 *   - Sets file->offset = 0
 *
 * File descriptor allocation:
 *   alloc_fd walks t->fds[0..MAX_FDS-1] and returns the first NULL slot.
 *   This is a first-fit allocator; no attempt to reuse closed fds.
 *
 * Called from: SYS_OPEN syscall handler
 *
 * Example:
 *   int fd = fd_open(current_task->fds, "/dev/console");
 *   if (fd >= 0) { ... use fd ... }
 */
int fd_open(fd_table_t *t, const char *path);

/*
 * int fd_read(fd_table_t *t, int fd, void *buf, size_t count)
 *
 * Read from an open file descriptor.
 *
 * Parameters:
 *   t: File descriptor table
 *   fd: File descriptor number
 *   buf: Destination buffer
 *   count: Number of bytes to read
 *
 * Returns:
 *   Number of bytes read on success (0–count, or 0 for EOF)
 *   -1 on error:
 *     - fd out of range [0, MAX_FDS)
 *     - t->fds[fd] is NULL (fd not open)
 *     - vnode->ops is NULL or ops->read is NULL
 *
 * Side effects:
 *   - Calls vnode->ops->read(vnode, file, buf, count)
 *   - Most devices do NOT advance file->offset; only block device does
 *   - For character devices (console, rng), offset is ignored
 *
 * Called from: SYS_READ syscall handler
 */
int fd_read(fd_table_t *t, int fd, void *buf, size_t count);

/*
 * int fd_write(fd_table_t *t, int fd, const void *buf, size_t count)
 *
 * Write to an open file descriptor.
 *
 * Parameters:
 *   t: File descriptor table
 *   fd: File descriptor number
 *   buf: Source buffer
 *   count: Number of bytes to write
 *
 * Returns:
 *   Number of bytes written on success (typically count)
 *   -1 on error:
 *     - fd out of range [0, MAX_FDS)
 *     - t->fds[fd] is NULL (fd not open)
 *     - vnode->ops is NULL or ops->write is NULL
 *
 * Side effects:
 *   - Calls vnode->ops->write(vnode, file, buf, count)
 *   - Block device advances file->offset; char devices ignore it
 *
 * Called from: SYS_WRITE syscall handler
 */
int fd_write(fd_table_t *t, int fd, const void *buf, size_t count);

/*
 * int fd_close(fd_table_t *t, int fd)
 *
 * Close a file descriptor.
 *
 * Parameters:
 *   t: File descriptor table
 *   fd: File descriptor number
 *
 * Returns:
 *   0 on success
 *   -1 on error (fd out of range, not open, etc.)
 *
 * Side effects:
 *   - kfree the file_t
 *   - Sets t->fds[fd] = NULL
 *   - Does NOT unmount or unregister vnodes
 *
 * Called from: SYS_CLOSE syscall handler
 */
int fd_close(fd_table_t *t, int fd);

/*
 * int64_t fd_seek(fd_table_t *t, int fd, int64_t offset, int whence)
 *
 * Seek to a position within an open file.
 *
 * Parameters:
 *   t: File descriptor table
 *   fd: File descriptor number
 *   offset: Offset value
 *   whence: SEEK_SET (0), SEEK_CUR (1), or SEEK_END (2)
 *
 * Returns:
 *   New absolute offset on success (>= 0)
 *   -1 on error:
 *     - fd out of range or not open
 *     - vnode is VNODE_CHR (char devices not seekable)
 *     - whence is not 0, 1, or 2
 *     - computed new_off < 0
 *     - (SEEK_END) vnode is not VNODE_REG (only regular files have size)
 *
 * Whence semantics:
 *   - SEEK_SET: new_off = offset
 *   - SEEK_CUR: new_off = file->offset + offset
 *   - SEEK_END: new_off = vnode->size + offset (requires VNODE_REG)
 *
 * Side effects:
 *   - Updates file->offset to new_off
 *
 * Note: Not exposed as a syscall (no SYS_SEEK); only used internally
 * by fat32_file_read, FAT32 directory traversal, etc.
 */
int64_t fd_seek(fd_table_t *t, int fd, int64_t offset, int whence);
```

---

## Implementation Details

### Vnode Pool and Allocation

```c
static vnode_t node_pool[MAX_VNODES];  // 128 * sizeof(vnode_t) bytes on heap (read-only BSS)
static int node_count = 0;             // Next available slot [0, MAX_VNODES)
```

- All vnodes are pre-allocated in a static array. No dynamic allocation of vnode_t itself.
- Allocation is trivial: `node_pool[node_count++]` (no free list, no fragmentation).
- If node_count reaches MAX_VNODES (128), further allocations fail.
- This limits the filesystem to 128 nodes; designs must account for this.
  - Root, /dev, /mnt, /mnt/fat32, /proc = 5 nodes
  - Device files (/dev/console, etc.) = 6 nodes
  - FAT32 files and directories = up to 111 nodes
  - Realistic limit: ~100 FAT32 entries

### Path Parsing and Traversal

The vfs_resolve function implements recursive descent:

1. Validate path starts with '/'
2. Initialize cur = root
3. For each '/' or component boundary:
   - Extract component name and length
   - Handle '.' (stay), '..' (ascend), or normal name (lookup)
   - Call find_child(cur, name, len) which:
     a. Scans cur->children linked list
     b. If found, return it
     c. Else if cur->v_ops && cur->v_ops->lookup, call it (lazy load)
     d. Else return NULL

Example walk of "/mnt/fat32/dir/file":
- cur = root, component = "mnt" → find_child → returns /mnt vnode
- cur = /mnt, component = "fat32" → find_child → returns /mnt/fat32 vnode
- cur = /mnt/fat32, component = "dir" → find_child → not in cache, call fat32_lookup → allocates vnode, loads metadata from disk, returns it
- cur = /mnt/fat32/dir, component = "file" → find_child → calls fat32_lookup → allocates vnode, returns it
- Return cur = /mnt/fat32/dir/file

### Memory and Concurrency

**Current (C version) implementation is NOT thread-safe:**
- Static vnode pool and node_count are not protected
- fd_table access is per-task, so no sharing (safe for single-tasking or per-task sync)
- No locks, no atomic operations

**Rust port implications:**
- Consider interior mutability (Mutex/RwLock) for global vnode tree if true multitasking is planned
- Per-task fd_table remains owned by task_t, no sharing between tasks
- vnode pool allocation can use a simple Mutex or atomic counter

### Device Integration Example: /dev/console

Devices are registered at boot via `devices_register()`:

```c
// devices.c
static int console_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n; (void)f;  // Ignore vnode and file offset
  unsigned char *p = buf;
  for (size_t i = 0; i < count; i++) {
    p[i] = uart_getc();  // Read one byte per call to UART driver
  }
  return (int)count;
}

static int console_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n; (void)f;
  const char *p = buf;
  for (size_t i = 0; i < count; i++) {
    uart_putc(p[i]);  // Write one byte per call to UART driver
  }
  return (int)count;
}

static file_operations_t console_ops = {
  .read = console_read,
  .write = console_write,
};

void devices_register(void) {
  vfs_register_chardev("console", &console_ops);
  // ...
}
```

When a user calls `sys_write(1, "Hello", 5)`:
1. Kernel extracts fd=1 from syscall
2. Calls fd_write(task->fds, 1, "Hello", 5)
3. Looks up task->fds->fds[1] → file_t
4. Calls file->vnode->ops->write(vnode, file, "Hello", 5)
5. This resolves to console_write, which calls uart_putc 5 times

### Block Device I/O: /dev/blk

The block device requires sector-aligned I/O:

```c
static int blk_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  
  // Sector alignment check: 512-byte sectors
  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;  // Enforce alignment
  }
  
  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  uint8_t *p = buf;
  
  for (size_t i = 0; i < sectors; i++) {
    if (blk_read(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }
  
  f->offset += (int64_t)count;  // **NOTE: Advances offset**
  return (int)count;
}
```

Key differences from character devices:
- Enforces 512-byte alignment on offset and count
- Manually increments f->offset after successful transfer
- Other devices (console, rng) ignore offset and do not advance it

### FAT32 Integration

FAT32 vnodes carry private_data pointing to FAT32 metadata:

```c
typedef struct fat32_priv {
  uint32_t first_cluster;   // Cluster where file/dir starts on disk
  uint32_t size;            // File size in bytes (0 for directories)
} fat32_priv_t;

static vnode_t *fat32_lookup(vnode_t *dir, const char *name, size_t namelen) {
  fat32_priv_t *pd = (fat32_priv_t *)dir->private_data;
  
  // ... lookup in FAT32 clusters on disk ...
  // On success:
  // - Allocate fat32_priv_t for the child
  // - Call vfs_create_node to link into tree
  // - Fill in child->private_data = cpd
  // - Fill in child->ops (for files) or child->v_ops (for dirs)
  // - Fill in child->size
  // Return child;
}
```

When vfs_resolve encounters a FAT32 path component not in cache, it calls fat32_lookup, which:
1. Parses the FAT32 cluster chain on disk
2. Looks up the named entry in the directory cluster
3. Allocates a new vnode
4. Fills in private_data with cluster metadata
5. Returns the vnode (now cached for future lookups)

---

## Boot and Initialization Order

1. **kernel.c early_main**: MMU + exceptions + heap
2. **vfs_init()**: Create root vnode, zero pool
3. **devices_register()**: Register /dev/console, /dev/null, /dev/zero, /dev/rng, /dev/blk, /dev/vcons
4. **fat32_mount()**: Load FAT32 root directory, fill in ops/v_ops
5. **vfs_create_node(root, "mnt", DIR)**: Create /mnt
6. **vfs_create_node(mnt, "fat32", DIR)**: Create /mnt/fat32
7. **fat32_vfs_mount("/mnt/fat32")**: Populate FAT32 tree
8. **proc_init()**: Register /proc files (uptime, meminfo, tasks, version, balloon)
9. **sched_init()**: Allocate per-task fd_tables via fd_table_create
10. **Create tasks**: Each gets its own fd_table
11. **Enter scheduler**: Tasks now call open/read/write syscalls

---

## Syscall Integration

VFS is invoked via four syscalls. The kernel's syscall handler (in exception.c or sched.c) dispatches to VFS:

```c
// kernel.c (syscall stubs for user code)
static inline int64_t sys_read(int fd, void *buf, uint64_t count) {
  register int x0 __asm__("x0") = fd;
  // ... set up x1, x2, x8 ...
  __asm__ __volatile__("svc #0" : "+r"(x0) : ...);
  return (int64_t)x0;
}

// exception handler (in sched.c or exception.c)
case SYS_READ: {  // x8 == 0
  int fd = (int)get_x0();
  void *buf = (void *)get_x1();
  uint64_t count = get_x2();
  int64_t ret = fd_read(current_task->fds, fd, buf, count);
  set_x0((uint64_t)ret);
  break;
}

case SYS_WRITE: {  // x8 == 1
  int fd = (int)get_x0();
  const void *buf = (const void *)get_x1();
  uint64_t count = get_x2();
  int64_t ret = fd_write(current_task->fds, fd, buf, count);
  set_x0((uint64_t)ret);
  break;
}

case SYS_OPEN: {  // x8 == 2
  const char *path = (const char *)get_x0();
  int64_t fd = fd_open(current_task->fds, path);
  set_x0((uint64_t)fd);
  break;
}

case SYS_CLOSE: {  // x8 == 3
  int fd = (int)get_x0();
  int64_t ret = fd_close(current_task->fds, fd);
  set_x0((uint64_t)ret);
  break;
}
```

System call numbers:
- `SYS_READ = 0`
- `SYS_WRITE = 1`
- `SYS_OPEN = 2`
- `SYS_CLOSE = 3`

---

## Rust Module Structure

### Proposed Layout

```
src/fs/vfs/
├── mod.rs                 // Public API (vfs_init, vfs_root, vfs_resolve, etc.)
├── vnode.rs              // vnode_t, vnode_type_t, tree traversal
├── file.rs               // file_t, file_operations_t vtable
├── fd_table.rs           // fd_table_t, fd_open/read/write/close/seek
└── allocator.rs          // Static vnode pool, alloc_vnode
```

### Typing Strategy

**Immutability-first design:**
- vnodes are mostly read-only once created
- Lazy lookups (FAT32, /proc) require interior mutability for cache population
- Consider: `Arc<Mutex<VnodeTree>>` for the global tree, or per-vnode `Mutex<Vec<Child>>`

**File descriptor table:**
- One fd_table per task (owned by task_t)
- Could use `Vec<Option<Arc<File>>>` with Mutex per entry, or simple array with task-level lock
- Simpler: `[Option<Box<File>>; MAX_FDS]` (no heap chasing for fds array itself)

**Vtables:**
- Use trait objects: `&dyn FileOperations`, `&dyn VnodeOperations`
- Or keep explicit function pointers for compatibility with device drivers written in C (not needed for pure Rust)
- Trait approach cleaner: `impl FileOperations for ConsoleDevice { fn read(...) {...} }`

### Concurrency Considerations

**Per-task fd_table:**
- No shared access between tasks (each task has its own)
- fd_seek and fd_read/fd_write update file->offset
- If multiple syscalls from the same task execute concurrently (unlikely in a simple scheduler), a Mutex per file might be needed
- For now: assume single-threaded per-task execution

**Global vnode tree:**
- All tasks navigate the same tree
- Lazy lookups (FAT32, /proc) mutate the tree (add cache entries)
- Solution: `RwLock<VnodeTree>` to allow concurrent reads, exclusive writes
- Or: per-vnode Mutex for the children list, allowing independent path walks

**Statics:**
- `static VNODE_POOL: [vnode_t; MAX_VNODES]` – immutable after init
- `static mut NODE_COUNT: usize` – unsafe, needs synchronization or atomic
- Better: `static NODE_COUNT: AtomicUsize` for allocation

### Static Size and Allocation

- `vnode_t` size: ~128 bytes (name[64] + type[4] + pointers + int64)
- `MAX_VNODES = 128` → pool ~16 KB (acceptable BSS)
- No dynamic vnode allocation; preallocated
- private_data and children linked lists are heap-allocated or external

### Ownership and Lifetimes

```rust
// Simplified structure

pub struct Vnode {
    name: [u8; 64],
    vnode_type: VnodeType,
    ops: Option<&'static dyn FileOperations>,
    v_ops: Option<&'static dyn VnodeOperations>,
    private_data: *mut c_void,
    size: u64,
    parent: Option<&'static Vnode>,
    children: Mutex<LinkedList<Box<Vnode>>>,  // Or just Vec for simplicity
    // ...
}

pub struct File {
    vnode: &'static Vnode,
    offset: i64,
}

pub struct FdTable {
    fds: [Option<Box<File>>; 64],
}
```

**Issues with 'static lifetime:**
- Vnodes are allocated from static pool, but children/linked-lists are dynamic
- Consider: `&'a Vnode` with arena allocator, or `Arc<Vnode>` for shared ownership
- Pragmatic: use raw pointers with unsafe { } and document invariants
- Or: use indices into a global vnode table (e.g., `VnodeId(usize)`)

---

## Hardware Details and Magic Numbers

VFS is purely software; no hardware registers or DMA.

### Per-Sector Block I/O

```c
#define SECTOR 512  // Sector size in bytes (constant for x86 and AArch64)
```

Block device operations align to 512-byte sectors. This is passed to the block layer (blk_read, blk_write) which talks to the underlying block device (VirtIO BLKK or QEMU simulated drive).

### Memory Allocator Constants

VFS uses kmalloc/kfree from the heap subsystem:

```c
#define HEAP_INITIAL_PAGES 256
#define HEAP_ALIGN 16
#define BLOCK_MAGIC_ALLOC 0xA110CEDUL
#define BLOCK_MAGIC_FREE  0xFEEDF1EEUL
```

File descriptor tables and file_t structures are heap-allocated. The heap is initialized early (before vfs_init) and available for allocation.

---

## Subtle Correctness Issues and Gotchas

### 1. Offset Handling Inconsistency

**Gotcha:** Not all devices advance file->offset after read/write.

- **Character devices** (console, rng): Ignore offset completely
- **Block device** (/dev/blk): Manually advances offset
- **Regular files** (FAT32): Offset is managed by the caller (FAT32's fat32_file_read does not advance it)

This is error-prone. When porting:
- Make offset semantics explicit in FileOperations trait
- Consider: enforce that ops->read always advances offset, and return value is bytes actually transferred
- Or: make offset management a responsibility of the fd_read/fd_write wrapper, not individual drivers

### 2. Vnode Pool Exhaustion

**Gotcha:** If MAX_VNODES (128) is exceeded, alloc_vnode returns NULL and vfs_create_node propagates the failure.

Scenarios that can exhaust the pool:
- Large directory trees
- Dynamic device registration (unlikely, but possible if implementing hotplug)
- Unbounded /proc file generation (currently limited, but future risk)

**No recovery:** Once exhausted, no new vnodes can be created for the lifetime of the system. Consider:
- Increase MAX_VNODES if FAT32 has many files
- Implement deferred freeing (remove children not accessed in N seconds)
- Or use a heap-based allocator instead of static pool

### 3. Name Truncation

**Gotcha:** Vnode names are truncated to 63 characters (name[64], leaving one for NUL).

```c
for (int i = 0; i < 63 && name[i]; i++) {
  n->name[i] = name[i];
}
```

If a device or file has a name longer than 63 chars, it will silently truncate. Collisions are unlikely but possible (e.g., "very_long_device_name_1" vs. "very_long_device_name_2" both truncate to the same prefix).

### 4. Parent Pointer Validity

**Gotcha:** Vnodes store a parent pointer, but there's no reference counting. If a parent vnode is somehow deallocated (not done in current code, but a risk in Rust), dereferencing parent->children could panic.

**Current assumption:** Vnodes are never freed; the pool is immutable after init. Port must preserve this guarantee or introduce refcounting.

### 5. Infinite Loop in .. Traversal

**Gotcha:** `..` traversal calls `cur = cur->parent`. If the root's parent is accidentally set to non-NULL (should be NULL), `..` from root could loop forever or access wild memory.

**Current safeguard:** `if (cur->parent) { cur = cur->parent; }` – only ascends if parent is non-NULL. Root is created with parent=NULL, so it's safe.

### 6. Path Parsing Edge Cases

**Gotcha:** Double slashes and trailing slashes are handled.

- `vfs_resolve("/dev//console")` – The code skips consecutive slashes, so this resolves correctly.
- `vfs_resolve("/dev/console/")` – After resolving "console", path points to the trailing '/', which is skipped, and the loop exits. Returns console vnode (correct).
- `vfs_resolve("/")` – After skipping the leading '/', path is empty. The loop doesn't enter. Returns root (correct).

### 7. Lazy Lookup Side Effects

**Gotcha:** Calling vfs_resolve with a FAT32 path mutates the tree (adds cached vnodes). This is not thread-safe.

If two tasks call vfs_resolve on the same FAT32 path concurrently:
- Both call find_child, both miss the cache, both call fat32_lookup
- Both allocate a vnode and try to link it into the tree
- Whichever executes second will see the first's vnode already cached
- But both allocate, and one's allocation is wasted

**Solution:** Protect vnode tree with a lock, or ensure single-threaded path resolution.

### 8. Vnode Linking Order

**Gotcha:** vfs_create_node prepends new children onto parent->children:

```c
if (parent) {
  n->next = parent->children;
  parent->children = n;
}
```

This means the last-created child is first in the list. If multiple devices are registered, walking children will see them in reverse order. This matters if code assumes FIFO or any particular traversal order.

Most code doesn't care; VFS resolves by name, not by iteration order.

### 9. File Operations Null Checks

**Gotcha:** fd_read/fd_write check if ops and ops->read/write are non-NULL:

```c
if (!f->vnode->ops || !f->vnode->ops->read) {
  return -1;
}
```

But directories (VNODE_DIR) have ops=NULL by design (they don't support read/write). Trying to read a directory returns -1. This is correct POSIX behavior, but easy to miss during porting.

### 10. File Descriptor Reuse

**Gotcha:** alloc_fd uses a simple first-fit scan; it doesn't reuse closed fds efficiently.

```c
static int alloc_fd(fd_table_t *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (!t->fds[i]) {
      return i;
    }
  }
  return -1;
}
```

If a task opens fd 0, closes it, then opens another, it gets fd 1 (not fd 0 again). After many open/close cycles, fds can be fragmented. With only 64 fds available, this is not a performance issue, but it's worth noting for reference implementations or future optimization.

---

## Assembly Required (None for VFS)

VFS is pure C and requires no assembly. All operations are:
- Tree traversal and node allocation
- String parsing (memcpy, strcmp)
- Function pointer dispatch
- Heap allocation/freeing

The only asm is in device drivers (UART, block device) and syscall entry (which calls into VFS after extracting registers).

---

## Summary

VFS is a **unified I/O dispatch layer** that abstracts over devices, filesystems, and pseudo-filesystems. It implements:

1. **Vnode tree** – in-memory filesystem tree with lazy loading
2. **Path resolution** – recursive descent with .. and . support
3. **Vtable dispatch** – file_operations_t and vnode_operations_t for polymorphism
4. **File descriptor table** – per-process fd array with offset tracking
5. **Device integration** – character devices, block devices, FAT32 directories, /proc files

The Rust port must preserve:
- Exact constant values (MAX_VNODES=128, MAX_FDS=64, SEEK_SET/CUR/END, SECTOR=512)
- Vnode tree semantics (parent/children linked list, name caching, lazy lookup)
- File offset tracking (especially for block devices which advance offset)
- Null checks and error handling (fd validation, path resolution failures, device ops availability)

Key porting decisions:
- Use `Arc<Mutex<Vnode>>` or a global RwLock for concurrent tree access
- Consider `&'static` or index-based references (VnodeId) instead of raw pointers
- Implement FileOperations and VnodeOperations as Rust traits
- Use `Vec<Option<Box<File>>>` or array for fd_table
- Ensure atomic vnode allocation via AtomicUsize or similar

