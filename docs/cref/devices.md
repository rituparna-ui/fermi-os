# Devices Subsystem Porting Spec

## Overview

The devices subsystem (`src/devices/devices.c` and `src/devices/devices.h`) provides virtual character and block device abstractions within the Fermi OS kernel. It registers six virtual devices under `/dev/` that userspace can access via the VFS (Virtual File System):

- **Character Devices:**
  - `/dev/console` – UART interface (stdin/stdout/stderr)
  - `/dev/null` – discard writes, EOF on reads
  - `/dev/zero` – infinite zero bytes on reads, discard writes
  - `/dev/rng` – random bytes from virtio-rng
  - `/dev/vcons` – virtio-console TX (host debug logging)
  
- **Block Device:**
  - `/dev/blk` – raw virtio-blk device with sector-aligned 512-byte I/O

All devices implement the VFS `file_operations_t` callback interface (defined in `src/fs/vfs/vfs.h`), which provides per-vnode read/write polymorphism. Each device's read/write handlers are registered with the kernel at boot via `devices_register()`.

---

## Public API

### Single Entry Point

```c
void devices_register(void)
```

**Signature:** No parameters, void return.

**Behavior:**
- Called once during kernel initialization (in `kernel_main` after `vfs_init()` and before userspace task creation).
- Registers six devices with the VFS:
  1. `vfs_register_chardev("console", &console_ops)`
  2. `vfs_register_chardev("null", &null_ops)`
  3. `vfs_register_chardev("zero", &zero_ops)`
  4. `vfs_register_chardev("rng", &rng_ops)`
  5. `vfs_register_chardev("vcons", &vcons_ops)`
  6. `vfs_register_blockdev("blk", &blk_ops)`
- Each registration creates a vnode in `/dev/` and installs the respective file operations struct.
- The call itself performs no error checking—failures in individual registrations would surface as vnode creation failures in the VFS layer.

**Invocation context** (from `kernel.c`):
```c
vfs_init();
devices_register();  // Called here in kernel_main after VFS is ready
```

---

## Underlying Types and VFS Integration

### From `src/fs/vfs/vfs.h`

```c
typedef struct file_operations {
  int (*read)(struct vnode *node, struct file *f, void *buf, size_t count);
  int (*write)(struct vnode *node, struct file *f, const void *buf, size_t count);
} file_operations_t;

typedef struct vnode {
  char name[64];
  vnode_type_t type;
  file_operations_t *ops;    // Function pointer table for this vnode
  vnode_operations_t *v_ops; // Lookup vtable (for directories)
  void *private_data;        // Filesystem/driver state (unused by devices)
  uint64_t size;             // Unused for devices
  struct vnode *parent;      // Backlink in tree
  struct vnode *children;    // Child vnodes (for dirs)
  struct vnode *next;        // Sibling link
} vnode_t;

typedef struct file {
  vnode_t *vnode;
  int64_t offset;            // Current file offset (maintained by VFS layer)
} file_t;
```

**Key points:**
- Each device registers an instance of `file_operations_t` with two function pointers: `read` and `write`.
- Userspace syscalls (`read(fd, buf, count)`, `write(fd, buf, count)`) land in the VFS layer, which looks up the vnode's `ops->read(...)` or `ops->write(...)`.
- The `file_t` struct holds the current seek offset; each read/write operation receives the current `file_t` state and may update the offset.
- `vnode_t.type` is set to `VNODE_CHR` (value 2) or `VNODE_BLK` (value 3) depending on device type.

---

## Per-Device Specification

### 1. `/dev/console` (UART character device)

**Type:** Character device (`VNODE_CHR`)

**Operations:**
```c
static int console_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  // Read count bytes from UART
  unsigned char *p = buf;
  for (size_t i = 0; i < count; i++) {
    p[i] = uart_getc();  // Blocks until char available
  }
  return (int)count;
}

static int console_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  // Write count bytes to UART
  const char *p = buf;
  for (size_t i = 0; i < count; i++) {
    uart_putc(p[i]);  // Blocks if TX FIFO full
  }
  return (int)count;
}
```

**Behavior:**
- **Read:** Blocks on each byte via `uart_getc()` until count bytes have been read. Returns count on success.
- **Write:** Writes each byte via `uart_putc()`, blocking if the UART TX FIFO is full. Returns count on success.
- **Offset:** Ignored; console I/O is not seekable (file_t.offset is not updated).
- **Error handling:** None; both operations assume success (no error returns).
- **VFS vnode parameters:** `vnode->ops = &console_ops`

**UART Backing (from `src/lib/uart/uart.h`):**
```c
#define UART_BASE     0x09000000UL   // PL011 UART MMIO base
#define UART_DR       (UART_BASE + 0x00)  // Data register
#define UART_FR       (UART_BASE + 0x18)  // Flag register
#define UART_ICR      (UART_BASE + 0x44)  // Interrupt clear register
#define UART_IBRD     (UART_BASE + 0x24)  // Integer baud divisor
#define UART_FBRD     (UART_BASE + 0x28)  // Fractional baud divisor
#define UART_LCRH     (UART_BASE + 0x2C)  // Line control register H
#define UART_CR       (UART_BASE + 0x30)  // Control register

void uart_init(void);
void uart_putc(const char c);
uint8_t uart_getc(void);
```

- `uart_getc()` polls `UART_FR` for RXFE (bit 4) until clear, then reads `UART_DR` and returns the byte.
- `uart_putc()` polls `UART_FR` for TXFF (bit 5) until clear, then writes to `UART_DR`.

---

### 2. `/dev/null` (Null device)

**Type:** Character device (`VNODE_CHR`)

**Operations:**
```c
static int null_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  return 0;  // Always EOF
}

static int null_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  return (int)count;  // Accept all writes, discard silently
}
```

**Behavior:**
- **Read:** Always returns 0 (EOF), regardless of count or current state. buf is not modified.
- **Write:** Always returns count (all bytes "accepted"), but they are discarded; no I/O occurs.
- **Offset:** Not updated.
- **VFS vnode parameters:** `vnode->ops = &null_ops`

---

### 3. `/dev/zero` (Zero device)

**Type:** Character device (`VNODE_CHR`)

**Operations:**
```c
static int zero_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  unsigned char *p = buf;
  for (size_t i = 0; i < count; i++) {
    p[i] = 0;
  }
  return (int)count;
}

static int zero_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  return (int)count;  // Accept and discard
}
```

**Behavior:**
- **Read:** Fills buf with count zero bytes, then returns count. An infinite source of zeros.
- **Write:** Always returns count (all bytes "accepted"), but they are discarded.
- **Offset:** Not updated.
- **VFS vnode parameters:** `vnode->ops = &zero_ops`

---

### 4. `/dev/rng` (Random number generator)

**Type:** Character device (`VNODE_CHR`)

**Operations:**
```c
static int rng_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  return rng_read(buf, (uint32_t)count);
}

static file_operations_t rng_ops = {
    .read = rng_dev_read,
    .write = NULL,  // No write operation
};
```

**Behavior:**
- **Read:** Delegates to `rng_read(buf, count)` from the virtio-rng driver. Returns the number of bytes actually read (≤ count), or negative on error.
- **Write:** Not supported (NULL in ops).
- **Offset:** Not updated.
- **VFS vnode parameters:** `vnode->ops = &rng_ops`

**RNG Backing (from `src/pci/virtio/rng/rng.h`):**
```c
#define VIRTIO_RNG_VENDOR_ID 0x1AF4
#define VIRTIO_RNG_DEVICE_ID 0x1044

int rng_read(void *buf, uint32_t count);  // Returns bytes read or error code
void pci_virtio_rng_init(void);  // Initialize at boot
```

---

### 5. `/dev/vcons` (Virtio-console TX)

**Type:** Character device (`VNODE_CHR`)

**Operations:**
```c
static int vcons_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  return 0;  // Always EOF; RX path not yet implemented
}

static int vcons_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  int n2 = vcons_send(buf, (uint32_t)count);
  return (n2 < 0) ? -1 : n2;
}
```

**Behavior:**
- **Read:** Always returns 0 (EOF). The RX (host → guest) virtqueue is not posted to, so incoming data is not delivered to userspace yet.
- **Write:** Sends count bytes down the virtio-console TX virtqueue to the host. Returns the number of bytes sent on success, or -1 if the driver is not ready.
- **Offset:** Not updated.
- **VFS vnode parameters:** `vnode->ops = &vcons_ops`

**Virtio-Console Backing (from `src/pci/virtio/console/console.h`):**
```c
#define VIRTIO_CONSOLE_VENDOR_ID   0x1AF4
#define VIRTIO_CONSOLE_DEVICE_ID   0x1043
#define VIRTIO_CONSOLE_VQ_RX 0     // Host → guest (not used)
#define VIRTIO_CONSOLE_VQ_TX 1     // Guest → host (used)

int vcons_send(const void *buf, uint32_t len);  // Push to TX queue
void pci_virtio_console_init(void);  // Initialize at boot
```

- Output appears in the host file specified on QEMU cmdline: `-chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt`
- Used for debug logging from userspace (e.g., `vlog` shell command writes to this device).

---

### 6. `/dev/blk` (Raw block device)

**Type:** Block device (`VNODE_BLK`)

**Constants:**
```c
#define SECTOR 512  // Sector size in bytes
```

**Operations:**
```c
static int blk_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  // Validate sector alignment
  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }
  
  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  uint8_t *p = buf;
  
  for (size_t i = 0; i < sectors; i++) {
    if (blk_read(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }
  
  f->offset += (int64_t)count;
  return (int)count;
}

static int blk_dev_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  // Validate sector alignment
  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }
  
  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  const uint8_t *p = buf;
  
  for (size_t i = 0; i < sectors; i++) {
    if (blk_write(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }
  
  f->offset += (int64_t)count;
  return (int)count;
}
```

**Behavior:**
- **Read:** 
  - Requires `f->offset % 512 == 0` and `count % 512 == 0`; returns -1 if not satisfied.
  - Converts byte offset and count to sector units.
  - Reads count bytes (in 512-byte chunks) from the block device starting at sector `f->offset / 512`.
  - Updates `f->offset` by count on success.
  - Returns count on success, -1 on I/O error.
  
- **Write:** 
  - Same alignment requirements as read.
  - Writes count bytes (in 512-byte chunks) to the block device starting at sector `f->offset / 512`.
  - Updates `f->offset` by count on success.
  - Returns count on success, -1 on I/O error.

- **Offset:** Updated by the number of bytes transferred. The VFS layer maintains this state.

- **VFS vnode parameters:** `vnode->ops = &blk_ops`, `vnode->type = VNODE_BLK`

**Block Device Backing (from `src/pci/virtio/blk/blk.h`):**
```c
#define VIRTIO_BLK_VENDOR_ID    0x1AF4
#define VIRTIO_BLK_DEVICE_ID    0x1042
#define VIRTIO_BLK_SECTOR_SIZE  512
#define VIRTIO_BLK_CFG_CAPACITY 0x00   // Device config offset for capacity in sectors
#define VIRTIO_BLK_T_IN  0             // Read request type
#define VIRTIO_BLK_T_OUT 1             // Write request type
#define VIRTIO_BLK_S_OK  0             // Success status
#define VIRTIO_BLK_S_IOERR 1           // I/O error status
#define VIRTIO_BLK_S_UNSUPP 2          // Unsupported status

struct virtio_blk_req {
  uint32_t type;      // VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT
  uint32_t reserved;
  uint64_t sector;    // Sector number
};

int blk_read(uint64_t sector, void *buf);   // Read one 512-byte sector
int blk_write(uint64_t sector, const void *buf);  // Write one 512-byte sector
void pci_virtio_blk_init(void);  // Initialize at boot
```

- Each `blk_read(sector, buf)` and `blk_write(sector, buf)` call operates on exactly 512 bytes.
- Returns `ESUCCESS` (value 1) on success, or an error code (0) on failure.
- Used for low-level disk I/O; FAT32 filesystem mounts on top of this.

---

## Error Codes

From `src/lib/utils/utils.h`:
```c
#define ESUCCESS 1
#define EERROR   0
```

All block device I/O functions return these codes. Character devices generally do not return errors (they assume success or block indefinitely).

---

## Kernel Integration & Boot Sequence

### Initialization Order (from `kernel.c`)

```c
void kernel_main() {
  // ... earlier init ...
  
  heap_init();
  gic_init();
  
  // Initialize PCI devices (provides rng, blk, vcons backing)
  pci_enumerate_bus();
  pci_virtio_rng_init();     // Initializes RNG driver
  pci_virtio_blk_init();     // Initializes block device driver
  pci_virtio_net_init();     // (separate from devices.c)
  pci_virtio_balloon_init(); // (separate from devices.c)
  pci_virtio_console_init(); // Initializes console driver
  
  // Initialize FAT32 filesystem (needs /dev/blk to be ready)
  if (fat32_mount() != ESUCCESS) {
    uart_printf("[FS][FAT32] Unable to mount file system");
  }
  
  // Initialize VFS tree
  vfs_init();
  
  // Register /dev/* devices (depends on vfs_init and backing drivers)
  devices_register();
  
  // Mount FAT32 at /mnt/fat32
  vnode_t *mnt = vfs_create_node(vfs_root(), "mnt", VNODE_DIR);
  vfs_create_node(mnt, "fat32", VNODE_DIR);
  fat32_vfs_mount("/mnt/fat32");
  
  // Initialize process/scheduler (now can open /dev/* devices)
  proc_init();
  sched_init();
  
  sched_create_task("task_a", task_a);
  sched_create_task("task_b", task_b);
  sched_create_task("task_shell", task_shell);
  sched_create_task("task_crash", task_crash);
  sched_create_kernel_task("netd", netd);
  
  // Start timer
  timer_init();
  timer_start(TIMER_INTERVAL_MS);
  
  uart_println("[KERNEL] Ready! running idle task...");
  while (1) {
    __asm__ __volatile__("wfi");  // Wait for interrupt
  }
}
```

**Critical ordering constraints:**
1. PCI drivers (`pci_virtio_*_init`) must be called before `devices_register()`.
2. `vfs_init()` must be called before `devices_register()` (to create /dev).
3. `devices_register()` must be called before any userspace task opens these devices.
4. Scheduler must be initialized after device registration to enable tasks to use the devices.

---

## Usage Examples (from `kernel.c`)

### Task B: Reading from `/dev/rng`
```c
int fd = sys_open("/dev/rng");
if (fd < 0) { /* error handling */ }

unsigned char r[4];
int64_t n = sys_read(fd, r, 4);  // Read 4 random bytes
if (n == 4) {
  // Process random bytes
}
```

### Task A: Reading from `/dev/console` (indirectly via stdin)
The shell task reads from `/dev/console` (fd=0) for user input:
```c
int64_t r = sys_read(0, &c, 1);  // Read 1 byte from stdin (/dev/console)
if (r > 0) { /* got character */ }
```

### Shell: Writing to `/dev/vcons` (debug log)
```c
int fd = sys_open("/dev/vcons");
const char *msg = "Hello from userspace!";
sys_write(fd, msg, strlen(msg));
```

---

## Rust Porting Strategy

### Module Structure

```rust
// src/devices/mod.rs
pub mod console;
pub mod null;
pub mod zero;
pub mod rng;
pub mod vcons;
pub mod blk;

pub fn devices_register() {
    // Call vfs_register_chardev/vfs_register_blockdev for each device
}
```

### Key Design Decisions

1. **File Operations Vtable:**
   - In C, `file_operations_t` is a struct of function pointers.
   - In Rust, implement as a trait with read/write methods, or use function pointers wrapped in statics.
   - Trait approach preferred for type safety; trait objects will be used to satisfy VFS expectations.

2. **Sector Alignment (Block Device):**
   - Use compile-time constant `const SECTOR_SIZE: usize = 512`.
   - Implement alignment checks as inline helper functions returning `Result<usize, DeviceError>`.
   - Leverage Rust's type system to avoid signed/unsigned casting errors.

3. **UART Backing:**
   - Leverage existing UART implementation; forward calls to `uart_getc()` / `uart_putc()`.
   - Mark as `extern "C"` to call C functions from the existing uart.c.
   - Wrap in safe Rust abstractions that handle blocking implicitly.

4. **Virtio Driver Integration:**
   - `/dev/rng`, `/dev/blk`, `/dev/vcons` depend on PCI drivers.
   - These will be separate modules (not in `devices.rs`).
   - `devices_register()` calls into their public APIs (rng_read, blk_read, blk_write, vcons_send).

5. **Offset Management:**
   - `file_t.offset` is maintained by the VFS layer, not the device.
   - Read/write handlers receive `&mut file` and may update offset in-place.
   - Block device MUST update offset after successful I/O.

6. **Error Handling:**
   - Use Rust `Result<T, E>` instead of C's -1 or special codes.
   - For VFS compatibility, map `Result` to `int` return values:
     - Success: return count (positive)
     - Failure: return -1 or error code (negative)
   - Character devices: generally assume success (like C version); block devices validate alignment.

7. **Statics for Immutable Function Tables:**
   - Each device type needs a static instance of file operations.
   - In Rust, use `lazy_static!` or `const` where possible.
   - Make these private and expose only via `devices_register()`.

### Type Mapping

| C Type               | Rust Equivalent              | Notes                                        |
|----------------------|------------------------------|----------------------------------------------|
| `int`                | `i32`                        | File ops return i32 (count or -1)           |
| `size_t`             | `usize`                      | Buffer sizes                                |
| `uint8_t / char`     | `u8`                         | Bytes                                       |
| `const void *buf`    | `&[u8]` or raw pointer       | Read buffer (must be safe)                  |
| `unsigned char *buf` | `&mut [u8]`                  | Write buffer target                         |
| `void *private_data` | Type parameter or trait obj  | VFS generic state (not used by devices)     |
| `vnode_t` / `file_t` | Foreign types (C-compatible) | Passed by reference from VFS layer          |

### Memory & Synchronization

- No dynamic allocation within device handlers (all operations are simple I/O forwarding).
- No locking needed within `/dev/console`, `/dev/null`, `/dev/zero` (purely local state).
- `/dev/rng`, `/dev/blk`, `/dev/vcons`: locking delegated to underlying PCI drivers.
- File offset updates are atomic relative to a single syscall (VFS ensures this).

### Assembly Requirements

- **None required** for devices.c itself. All operations delegate to:
  - UART driver (C): `uart_getc()`, `uart_putc()` – may use inline asm for MMIO.
  - PCI virtio drivers (C): `rng_read()`, `blk_read()`, `blk_write()`, `vcons_send()` – may use barriers.
  - No context switching, exception handling, or privileged register access needed by devices layer.

---

## Hardware Constants and Register Mappings

### UART (PL011)

```
Base Address: 0x09000000
DR   (offset 0x00): Data Register
FR   (offset 0x18): Flag Register (RXFE=bit4, TXFF=bit5)
ICR  (offset 0x44): Interrupt Clear Register
IBRD (offset 0x24): Integer Baud Divisor (value 13 for 115200)
FBRD (offset 0x28): Fractional Baud Divisor (value 2 for 115200)
LCRH (offset 0x2C): Line Control Register H (enable FIFO, 8-bit data, 1 stop)
CR   (offset 0x30): Control Register (enable UART, RX, TX)
```

Clock frequency: 24 MHz
Baud rate: 115200
Divisor formula: clk / (16 * baud) = 24000000 / (16 * 115200) ≈ 13.02

### Virtio PCI Devices

| Device       | Vendor ID | Device ID | Sector Size | Key Constants              |
|--------------|-----------|-----------|-------------|---------------------------|
| RNG          | 0x1AF4    | 0x1044    | N/A         | N/A                        |
| Block        | 0x1AF4    | 0x1042    | 512         | CAPACITY @0x00, T_IN/T_OUT |
| Console      | 0x1AF4    | 0x1043    | N/A         | VQ_TX=1, VQ_RX=0           |

---

## Gotchas & Subtleties

1. **Offset Handling in Block Device:**
   - The VFS layer initializes `file->offset = 0` on open.
   - After each read/write, the C code MUST update `f->offset += count`.
   - Rust port must mirror this exactly; offset is not reset and persists across syscalls.

2. **Sector Alignment Enforcement:**
   - Both offset and count must be multiples of 512 for block device.
   - Misaligned I/O returns -1 (error).
   - No partial-sector I/O allowed; this is a strict hardware requirement.

3. **UART Blocking:**
   - `uart_getc()` and `uart_putc()` spin-wait on FIFO flags.
   - If the backing device is missing or broken, they hang indefinitely.
   - No timeout or error recovery; C kernel assumes UART is always present.

4. **RNG/Block/VConsole Initialization Order:**
   - These require PCI enumeration to complete first.
   - If PCI driver init fails, the device handle is NULL or invalid.
   - `vcons_send()` returns -1 if driver not ready; `blk_read/write()` return error code if driver fails.
   - Userspace is responsible for handling ENODEV-style errors.

5. **Write vs. File Buffer Truncation:**
   - `/dev/null`, `/dev/zero` claim to accept all writes but discard them.
   - This is correct for the intended use case (bit bucket).
   - Real filesystems would truncate large writes to fit; devices do not.

6. **Virtio-Console RX Not Implemented:**
   - RX virtqueue is not posted; incoming data from host is silently dropped.
   - `/dev/vcons` read always returns 0 (EOF).
   - Future work: post RX buffers and deliver to userspace on read.

7. **Count Parameter Width:**
   - Syscall passes `uint64_t count`, but device handlers cast to `size_t` (typically `usize` on 64-bit).
   - Block device read/write loop: `for (size_t i = 0; i < sectors; i++)` — be careful with overflow if count is huge.
   - In practice, QEMU and VFS enforce reasonable limits.

8. **Signedness:**
   - File operations return `int` (can be negative for error).
   - Count is `size_t` (unsigned); casting return to `int` requires care (large counts will wrap).
   - In practice, reasonable buffer sizes (< 2^31) avoid this.

---

## Testing / Validation Checklist

- [ ] `/dev/console` read/write: basic echo via UART
- [ ] `/dev/null` read returns 0, write succeeds
- [ ] `/dev/zero` read returns zeros, write succeeds
- [ ] `/dev/rng` read returns random bytes
- [ ] `/dev/blk` sector-aligned read/write; misaligned returns -1
- [ ] `/dev/vcons` write sends to host; read returns 0
- [ ] File offset updated correctly after I/O (especially block device)
- [ ] Multiple tasks can open the same device concurrently
- [ ] Shell `cat /dev/zero`, `hexdump /dev/zero` work (produces zero lines)
- [ ] Shell `cat /dev/rng` works (produces random bytes)
- [ ] Shell `vlog <msg>` writes to `/dev/vcons` and appears in host log

---

## Summary

The devices subsystem is a thin VFS adapter layer that registers six virtual devices. It forwards I/O requests to:
- UART driver (console, stdin/stdout)
- Virtio drivers (rng, blk, vcons)
- Simple algorithms (null, zero)

All handlers are simple, stateless, and synchronous. The Rust port should preserve the exact structure and error semantics of the C version while leveraging Rust's type system for safety (especially in the block device sector alignment logic).

