# /proc Subsystem (Synthetic Filesystem) — Porting Spec

## Overview

The `/proc` subsystem implements a synthetic read-only filesystem that exposes live kernel state through regular file reads. Each `/proc` file regenerates its content on every read from live kernel state snapshots; nothing is persisted. The subsystem wraps existing kernel subsystem APIs (timer, PMM, heap, scheduler, GIC, network, balloon, CPU) and presents them as human-readable text files.

### Design Philosophy

- **Dynamic Generation**: Every read call triggers a generator function to snapshot live kernel state.
- **Stateless**: File offset and EOF semantics are handled uniformly by a wrapper (`proc_read_via`).
- **Fixed Buffer**: Each generator writes into a stack-local 2048-byte buffer; truncation occurs silently if the snapshot exceeds the buffer.
- **No Persistence**: All content is computed on demand; closing and reopening a file sees fresh state.

### Files Exposed

```
/proc/
├── uptime         System uptime in seconds.hundredths
├── meminfo        Physical memory and heap statistics (KB)
├── tasks          Scheduler task table (PID / state / name)
├── version        Kernel banner and build timestamp
├── balloon        Virtio-balloon page counts
├── netinfo        Network device statistics
├── interrupts     GIC interrupt statistics
├── cmdline        Kernel command-line arguments (stub)
└── cpuinfo        CPU information
```

---

## Architecture

### Constants

```c
#define PROC_BUF_BYTES 2048       /* Max bytes per generator snapshot */
```

All generator functions write into a 2048-byte temporary buffer. If the snapshot exceeds this size, `kvsnprintf` silently truncates it.

### Generator Function Signature

```c
typedef int (*proc_generator_fn)(char *buf, size_t buflen);
```

**Returns**: Number of bytes written (or would be written) by `kvsnprintf`. A negative return (-1) indicates error.

---

## Public API

### Initialization Function

#### `void proc_init(void)`

**Signature**: 
```c
void proc_init(void);
```

**Behavior**:
1. Creates the `/proc` directory at the filesystem root via `vfs_create_node(vfs_root(), "proc", VNODE_DIR)`.
2. Registers 9 files under `/proc`:
   - `uptime` → `read_uptime` → `gen_uptime`
   - `meminfo` → `read_meminfo` → `gen_meminfo`
   - `tasks` → `read_tasks` → `gen_tasks`
   - `netinfo` → `read_netinfo` → `gen_netinfo`
   - `interrupts` → `read_interrupts` → `gen_interrupts`
   - `cmdline` → `read_cmdline` → `gen_cmdline`
   - `version` → `read_version` → `gen_version`
   - `balloon` → `read_balloon` → `gen_balloon`
   - `cpuinfo` → `read_cpuinfo` → `gen_cpuinfo`
3. Logs progress to UART via `uart_println()` and error messages via `uart_errorln()`.

**Call Site**: 
- Invoked from `kernel.c` after VFS init (`vfs_init()`), FAT32 mount, and before scheduler init (`sched_init()`).
- **Ordering**: Must be called after VFS is initialized but the order relative to other driver initialization (devices, networking) does not matter.

**Errors**:
- If `/proc` directory creation fails: logs `"[PROC] Failed to create /proc"` and returns early.
- If any file creation fails: logs `"[PROC] Failed to create /proc/<filename>"` but continues registering remaining files.

---

## Generator Functions (Private Implementation)

All generator functions follow the same pattern:
1. Accept a `char *buf` and `size_t buflen`.
2. Write formatted output into `buf` using `ksnprintf()`.
3. Return the number of bytes written by `kvsnprintf()` (which returns the "would-write" count).
4. Never write beyond `buflen` (ksnprintf handles truncation).

### `static int gen_uptime(char *buf, size_t buflen)`

**Behavior**:
- Queries `timer_uptime_ms()` to get system uptime in milliseconds.
- Converts to seconds and hundredths of a second.
- Formats as: `"<seconds>.<hundredths>\n"` (e.g., `"1234.56\n"`).
- Example output: `"123.45\n"` for 123.45 seconds.

**Formula**:
```
ms = timer_uptime_ms()
s = ms / 1000
cs = (ms % 1000) / 10
output = "<s>.<cs/10><cs%10>\n"
```

**Dependencies**: `timer_uptime_ms()` from timer subsystem.

---

### `static int gen_meminfo(char *buf, size_t buflen)`

**Behavior**:
- Queries physical memory and heap stats from PMM and heap subsystems.
- Formats a multi-line output showing memory in kilobytes.
- 4 KiB pages are assumed (multiply by 4 to get KB).

**Output Format**:
```
MemTotal:    <total_pages * 4> KB
MemUsed:     <used_pages * 4> KB
MemFree:     <free_pages * 4> KB
MemReserved: <reserved_pages * 4> KB
HeapTotal:   <heap_total / 1024> KB
HeapUsed:    <heap_used / 1024> KB
HeapFree:    <heap_free / 1024> KB
```

**Dependencies**:
- `pmm_get_total_pages()` from PMM subsystem.
- `pmm_get_used_pages()` from PMM subsystem.
- `pmm_get_free_pages()` from PMM subsystem.
- `pmm_get_reserved_pages()` from PMM subsystem.
- `heap_used_bytes()` from heap subsystem.
- `heap_free_bytes()` from heap subsystem.
- `heap_total_bytes()` from heap subsystem.

**Constants** (from pmm.h):
```c
#define PAGE_SIZE 4096     /* 4 KiB pages */
```

---

### `static int gen_tasks(char *buf, size_t buflen)`

**Behavior**:
- Walks the circular task run queue starting from `sched_first_task()`.
- Prints a header row: `"PID  STATE     NAME\n"` followed by dashes.
- For each task, formats: `"<pid>  <state_name>   <name>\n"`.
- Stops if reaching end of buffer without error.

**Output Format**:
```
PID  STATE     NAME
---- --------- ----------------
1    RUNNING   task_a
2    READY     task_b
0    RUNNING   idle
```

**Loop Semantics**:
- The task list is circular: walk `t->next` until you loop back to the head.
- Start: `head = sched_first_task(); t = head`.
- Iteration: `t = t->next` until `t == head` or `t == NULL`.

**Dependencies**:
- `sched_first_task()` from scheduler subsystem (returns head of run queue).
- `task_state_name(task_state_t)` from scheduler subsystem (converts state enum to string).
- `task_t.pid`, `task_t.state`, `task_t.name` fields from scheduler subsystem.

---

### `static int gen_netinfo(char *buf, size_t buflen)`

**Behavior**:
- Delegates directly to `net_get_info(buf, buflen)` from the network subsystem.
- The network layer builds the snapshot directly into the provided buffer.
- The `proc_read_via()` wrapper handles offset and EOF semantics.

**Dependencies**:
- `net_get_info(char *buf, uint32_t buflen)` from network subsystem (returns byte count written).

---

### `static int gen_interrupts(char *buf, size_t buflen)`

**Behavior**:
- Delegates directly to `gic_render_interrupts(buf, buflen)` from the GIC subsystem.
- GIC renders interrupt statistics directly into the provided buffer.
- The `proc_read_via()` wrapper handles offset and EOF semantics.

**Dependencies**:
- `gic_render_interrupts(char *buf, uint32_t buflen)` from GIC subsystem (returns byte count written).

---

### `static int gen_cmdline(char *buf, size_t buflen)`

**Behavior**:
- Returns a stub kernel command line.
- Currently hard-coded; when boot-arg parsing is implemented, will surface actual passed arguments.
- Outputs: `"console=ttyAMA0 maxcpus=1 net=virtio-net-pci ip=10.0.2.15\n"`.

**Dependencies**: None (pure constant).

---

### `static int gen_version(char *buf, size_t buflen)`

**Behavior**:
- Outputs kernel version banner and build timestamp.
- Uses compiler macros `__DATE__` and `__TIME__` for build timestamp.

**Output Format**:
```
Fermi OS aarch64 (cortex-a72)
Built: <__DATE__> <__TIME__>
```

**Example**:
```
Fermi OS aarch64 (cortex-a72)
Built: Jun 25 2026 15:28:30
```

**Dependencies**: None (uses compiler macros).

---

### `static int gen_balloon(char *buf, size_t buflen)`

**Behavior**:
- Queries virtio-balloon actual and target page counts.
- Both counters are in 4 KiB pages; display as pages and KB (pages * 4).

**Output Format**:
```
actual:      <actual_pages> pages (<actual_pages * 4> KB)
host_target: <target_pages> pages (<target_pages * 4> KB)
```

**Dependencies**:
- `balloon_get_status(uint32_t *actual_pages, uint32_t *host_target)` from balloon subsystem (out parameters).

---

### `static int gen_cpuinfo(char *buf, size_t buflen)`

**Behavior**:
- Delegates directly to `cpu_render_info(buf, buflen)` from the CPU subsystem.
- CPU layer formats information (model, frequency, etc.) directly into the buffer.
- The `proc_read_via()` wrapper handles offset and EOF semantics.

**Dependencies**:
- `cpu_render_info(char *buf, size_t buflen)` from CPU subsystem (returns byte count written).

---

## Read Wrapper Pattern

### `static int proc_read_via(proc_generator_fn gen, file_t *f, void *buf, size_t count)`

**Behavior**:
1. Calls the generator `gen()` into a stack-local 2048-byte buffer.
2. Checks the generator's return value; returns -1 if generation failed.
3. Caps the available size at `PROC_BUF_BYTES - 1` (leave room for NUL terminator).
4. If the file offset is beyond the available data, returns 0 (EOF).
5. Copies bytes `[f->offset, min(f->offset + count, avail))` into the caller's buffer.
6. Updates `f->offset` and returns the number of bytes copied.

**Algorithm**:
```c
char tmp[2048];
int total = gen(tmp, sizeof(tmp));        // Call generator
if (total < 0) return -1;                  // Error handling

size_t avail = (size_t)total;              // Convert to unsigned
if (avail >= sizeof(tmp)) {
  avail = sizeof(tmp) - 1;                // Cap at buffer size (NUL)
}

if ((uint64_t)f->offset >= avail) {
  return 0;                                // EOF
}

size_t remaining = avail - (size_t)f->offset;
size_t to_copy = (count < remaining) ? count : remaining;
memcpy(buf, tmp + f->offset, to_copy);
f->offset += to_copy;
return (int)to_copy;
```

**File Offset Semantics**:
- On the first read at offset 0, copies bytes [0, min(count, avail)).
- Subsequent reads advance `f->offset` monotonically.
- When `f->offset >= avail`, further reads return 0 (EOF).
- The file is not seekable; only sequential reads work correctly.

---

## File Operations Tables

Each file has a static `file_operations_t` table:

```c
typedef struct file_operations {
  int (*read)(struct vnode *node, struct file *f, void *buf, size_t count);
  int (*write)(struct vnode *node, struct file *f, const void *buf, size_t count);
} file_operations_t;
```

All `/proc` files are read-only:

```c
static file_operations_t uptime_ops    = {.read = read_uptime,    .write = 0};
static file_operations_t meminfo_ops   = {.read = read_meminfo,   .write = 0};
static file_operations_t tasks_ops     = {.read = read_tasks,     .write = 0};
static file_operations_t netinfo_ops   = {.read = read_netinfo,   .write = 0};
static file_operations_t interrupts_ops = {.read = read_interrupts, .write = 0};
static file_operations_t cmdline_ops   = {.read = read_cmdline,   .write = 0};
static file_operations_t version_ops   = {.read = read_version,   .write = 0};
static file_operations_t balloon_ops   = {.read = read_balloon,   .write = 0};
static file_operations_t cpuinfo_ops   = {.read = read_cpuinfo,   .write = 0};
```

Each read wrapper function unwraps the vnode, calls `proc_read_via()` with the corresponding generator, and returns the result:

```c
static int read_uptime(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;  /* Unused; generator has all state it needs */
  return proc_read_via(gen_uptime, f, buf, count);
}
/* Similar for all 9 files */
```

---

## Registration Helper

### `static void register_file(vnode_t *parent, const char *name, file_operations_t *ops)`

**Behavior**:
1. Creates a new regular file vnode under `parent` with the given `name`.
2. Assigns the file operations table to `vnode->ops`.
3. Logs an error if creation fails but does not return (allows others to continue).

**Implementation**:
```c
vnode_t *n = vfs_create_node(parent, name, VNODE_REG);
if (!n) {
  uart_printf("[PROC] Failed to create /proc/%s\n", name);
  return;  /* Continue with next file */
}
n->ops = ops;
```

---

## Subsystem Dependencies

The `/proc` subsystem depends on (and calls into):

### Read-only dependencies (queries):

| Subsystem | Function(s) | Purpose |
|-----------|-------------|---------|
| **Timer** | `timer_uptime_ms()` | /proc/uptime |
| **PMM** | `pmm_get_total_pages()`, `pmm_get_used_pages()`, `pmm_get_free_pages()`, `pmm_get_reserved_pages()` | /proc/meminfo (physical memory) |
| **Heap** | `heap_used_bytes()`, `heap_free_bytes()`, `heap_total_bytes()` | /proc/meminfo (heap stats) |
| **Scheduler** | `sched_first_task()`, `task_state_name()`, task_t fields | /proc/tasks |
| **GIC** | `gic_render_interrupts()` | /proc/interrupts |
| **Network** | `net_get_info()` | /proc/netinfo |
| **Balloon** | `balloon_get_status()` | /proc/balloon |
| **CPU** | `cpu_render_info()` | /proc/cpuinfo |
| **VFS** | `vfs_root()`, `vfs_create_node()` | Directory/file creation |
| **Strings** | `ksnprintf()`, `kvsnprintf()`, `memcpy()` | Formatting |
| **UART** | `uart_printf()`, `uart_println()`, `uart_errorln()` | Diagnostics |

### Depended on by:

- **User shell** / **applications** reading `/proc/*` files via VFS read syscall.
- Any kernel subsystem that consumes `/proc/meminfo` or `/proc/tasks` diagnostics.

---

## Boot/Usage Ordering

### Initialization Order (from kernel.c)

```c
vfs_init();                    /* Initialize VFS root */
devices_register();            /* Register /dev devices */
fat32_vfs_mount("/mnt/fat32"); /* Mount FAT32 */
proc_init();                   /* Initialize /proc AFTER VFS ready */
sched_init();                  /* Scheduler AFTER /proc (not required, but conventional) */
```

### Prerequisites for `proc_init()`:

1. **VFS must be initialized**: `vfs_root()` must be callable and return the filesystem root.
2. **VFS node creation must work**: `vfs_create_node()` must function.
3. All queried subsystems should be initialized (timer, PMM, heap, scheduler, GIC, network, balloon, CPU).
   - If a subsystem is not yet initialized when a `/proc` file is read, the generator will call uninitialized functions and likely crash or return garbage.
   - In practice, the subsystems are initialized before `/proc/tasks`, `/proc/meminfo`, etc. are accessed, so this is not a runtime concern.

### Runtime Usage

- Each `/proc` file is read-only and safe to read from userspace or kernel diagnostics at any time.
- Reads are atomic with respect to each generator (no locking is required within a generator).
- Multiple concurrent reads to different files do not interfere (each generator is independent).
- Reading the same file multiple times yields fresh snapshots.

---

## Error Handling

### Initialization Errors

- If `/proc` directory creation fails, the entire `proc_init()` returns early.
- If a file creation fails, `register_file()` logs the failure and returns; subsequent files continue to be registered.
- All error messages go to UART via `uart_printf()` / `uart_errorln()` with the `"[PROC]"` prefix.

### Read Errors

- If a generator returns -1, `proc_read_via()` returns -1 to the caller.
- If a generator's return value exceeds 2048, the available data is silently capped at 2047 bytes (NUL terminator reserved).
- If the file offset exceeds the available data, the read returns 0 (EOF) without error.

### No Persistence Errors

- There are no persistence failures because `/proc` files are never written to persistent storage.
- All content is computed on demand and lost when the system reboots.

---

## Rust Port Strategy

### High-Level Structure

```rust
// proc/mod.rs (or proc.rs)
pub fn proc_init() -> Result<(), &'static str> {
  // Create /proc directory via vfs::create_node()
  // Register all 9 files with their ops tables
  // Return error if any critical step fails
}

// Generator functions: each returns Result<String, Error>
// Modern approach: use a format! or write! macro into a heap-allocated String
// or preallocate a [u8; 2048] on the stack and use write!()'s Cursor<> wrapper

mod generators {
  pub fn gen_uptime() -> Result<String, &'static str> { ... }
  pub fn gen_meminfo() -> Result<String, &'static str> { ... }
  // ... etc for all 9 generators
}

// File operations vtable wrappers
mod file_ops {
  // Each exposes a read_* function matching the VFS file_operations signature
  pub fn read_uptime(vnode: &Vnode, file: &mut File, buf: &mut [u8]) -> Result<usize, i32> { ... }
  // ... etc
}
```

### Locking Strategy

- **No explicit locking needed**: Each generator function is stateless and calls into other subsystems.
- **Shared state concern**: If a generator queries a subsystem that has mutable internal state (e.g., scheduler task list, GIC counters), ensure that subsystem provides thread-safe read accessors or use RwLock<T> / Mutex<T> as needed.
  - In the C kernel, these are guarded implicitly by single-threaded execution or CLI (disabling interrupts).
  - In Rust, translate this to explicit RwLock guards: hold a read lock for the duration of the generator query.
- **Recommended**: Each subsystem providing read-only stats should export a function like `pmm_get_total_pages()` that acquires and releases a lock internally (no lock needed in proc).

### Module Organization

```
src/fs/proc/
├── mod.rs          // Main init and file registration
├── generators.rs   // All 9 generator functions
└── file_ops.rs     // File operations wrappers (optionally inline in mod.rs)
```

### Type Mappings

| C Type | Rust Equivalent |
|--------|-----------------|
| `proc_generator_fn` | `Fn() -> Result<String, &'static str>` or `fn() -> Result<Vec<u8>, &'static str>` |
| `file_operations_t` | `struct FileOps { read: fn(...) -> i32, write: fn(...) -> i32 }` |
| `vnode_t` | `struct Vnode { ... ops: &FileOps, ... }` |
| `file_t` | `struct File { vnode: &Vnode, offset: i64 }` |

### String Formatting

- Use the `write!` macro with a `Cursor<Vec<u8>>` or a fixed-size array `[u8; 2048]`.
- Example:
  ```rust
  use std::io::Write;
  
  fn gen_uptime() -> Result<Vec<u8>, &'static str> {
    let ms = timer_uptime_ms();
    let s = ms / 1000;
    let cs = (ms % 1000) / 10;
    
    let mut buf = Vec::new();
    write!(buf, "{}.{}{}\n", s, cs / 10, cs % 10)
      .map_err(|_| "write failed")?;
    Ok(buf)
  }
  ```

- Or use a pre-allocated stack buffer:
  ```rust
  fn gen_uptime() -> Result<usize, &'static str> {
    let mut buf = [0u8; 2048];
    let n = write!(&mut Cursor::new(&mut buf[..]), "...")
      .map_err(|_| "write failed")?;
    Ok(n)
  }
  ```

### Error Handling in Rust

- Generators return `Result<String, &'static str>` or `Result<usize, &'static str>`.
- `proc_read_via` unwraps or propagates the error:
  ```rust
  pub fn proc_read_via(gen: fn() -> Result<Vec<u8>>, file: &mut File, buf: &mut [u8]) -> Result<usize> {
    let data = gen()?;  // If gen fails, propagate the error
    // ... offset and copy logic
  }
  ```

### Const Preservation

- Define the constant `PROC_BUF_BYTES` as a `const` in the module:
  ```rust
  pub const PROC_BUF_BYTES: usize = 2048;
  ```

### No Assembly Required

- The `/proc` subsystem is 100% portable Rust and does not require any assembly or sysreg access.
- All functionality is provided by other subsystems (timer, PMM, scheduler, etc.).

---

## Gotchas & Correctness Issues

1. **Generator Truncation**: If a generator's output exceeds 2048 bytes, `kvsnprintf` silently truncates. The file appears shorter than it should; there is no error signal. Document this limitation.

2. **Circular Task List**: The task list is circular; must detect the loop by comparing `t == head` or checking for NULL. The C code uses both conditions (`t && t != head`). In Rust, use an iterator with a cycle-detection guard.

3. **File Offset Semantics**: The file offset is not seekable. Seeking backward or to arbitrary positions does not work. Only sequential reads advance correctly. The VFS layer is responsible for enforcing this; `/proc` generators don't need to handle seeks explicitly.

4. **Stale Snapshots**: Between multiple reads, the underlying subsystem state can change. A single `/proc/meminfo` read reflects the state at the moment of the read; a second read sees a potentially different state. This is correct behavior for a dynamic /proc implementation.

5. **Generator Independence**: Generators don't share state. Each is atomic and independent. However, if a generator queries a subsystem with internal locks (e.g., scheduler), ensure the lock is held for the entire generator's duration or that the subsystem provides a snapshot function.

6. **Build Timestamp Format**: `__DATE__` and `__TIME__` are compiler macros; in Rust, use `env!("PROFILE")` and similar build-time environment introspection, or recompute them with `chrono` at runtime if dynamic builds are required.

7. **Command-Line Stub**: The `/proc/cmdline` is hard-coded and does not reflect actual boot arguments until boot-arg parsing is implemented. Update when that subsystem is ready.

8. **No Locking in Generators**: All generators assume they can safely call into other subsystems without holding locks. If a subsystem's query functions are not reentrant or not thread-safe, protect them with RwLock at the subsystem boundary, not in proc.

9. **Memory Budget**: The 2048-byte buffer per read is sufficient for all current generators. If adding new generators (e.g., disk I/O stats, network packet counts), verify that the output fits or increase the buffer size. Tuple or multi-call reads may be needed for large snapshots.

10. **UART Logging**: All initialization logs go to UART. Ensure UART is initialized before `proc_init()` is called. If it isn't, the logging calls will hang or crash. In Rust, wrap UART access in a safe interface.

---

## Hardware Details & Magic Constants

All hardware constants are delegated to subsystems. The `/proc` subsystem itself has no direct hardware access:

### Page Size (from PMM)
```c
#define PAGE_SIZE 4096      /* 4 KiB pages */
#define PAGE_SHIFT 12       /* log2(PAGE_SIZE) */
```

Conversions:
- KB = pages * 4
- pages = bytes / 4096

### Timer (from timer subsystem)
```c
#define TIMER_INTERVAL_MS 10  /* 10ms tick */
#define TIMER_PPI_INTID 30    /* PPI ID for ARM generic timer */
```

The `/proc/uptime` generator uses `timer_uptime_ms()` without needing these constants directly.

### Memory Map (from pmm.h)
```c
#define MEM_START 0x40000000ULL    /* Physical memory base */
#define MEM_SIZE (8ULL * 1024 * 1024 * 1024)  /* 8 GiB */
```

Not directly used by `/proc`, but documented for reference.

### UART Base Address (for logging)
```c
#define UART_BASE 0x09000000UL
```

Used by `uart_printf()` and similar functions called during `proc_init()`.

---

## Spec Completeness Checklist

- [x] Overview and purpose of the subsystem.
- [x] All 9 generator functions documented with behavior, dependencies, and constants.
- [x] `proc_init()` initialization function fully specified.
- [x] File operations pattern and all 9 read wrapper functions explained.
- [x] `proc_read_via()` wrapper algorithm and file offset semantics detailed.
- [x] Boot/usage ordering and prerequisites listed.
- [x] All subsystem dependencies documented.
- [x] Error handling for initialization and read failures.
- [x] Rust port strategy with type mappings, locking, and module organization.
- [x] Assembly: confirmed as not required (100% portable Rust).
- [x] Hardware constants and magic numbers from delegated subsystems.
- [x] Gotchas and correctness issues enumerated.

---

## References

- **C Source**: `src/fs/proc/proc.c`, `src/fs/proc/proc.h`
- **VFS Integration**: `src/fs/vfs/vfs.h`
- **Subsystems**:
  - Timer: `src/exception/timer/timer.h`
  - PMM: `src/mm/pmm/pmm.h`
  - Heap: `src/mm/heap/heap.h`
  - Scheduler: `src/sched/sched.h`
  - GIC: `src/exception/gic/gic.h`
  - Network: `src/pci/virtio/net/net.h`
  - Balloon: `src/pci/virtio/balloon/balloon.h`
  - CPU: `src/lib/cpu/cpu.h`
  - Strings: `src/lib/strings/strings.h`
  - UART: `src/lib/uart/uart.h`

