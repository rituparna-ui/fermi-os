# VirtIO Block Device (blk) Subsystem Specification

## Overview

The VirtIO block subsystem provides guest access to virtio-blk block devices (QEMU virtual disks). It implements a bare-metal driver that:
- Initializes a PCI-based VirtIO block device via the VirtIO 1.0 MMIO interface
- Configures a single virtqueue (queue 0) for I/O requests
- Provides sector-based read/write operations (512-byte sectors)
- Uses chained descriptor lists for request/response handling with volatile status polling

The driver runs at EL1 (kernel) and is invoked during early boot before filesystem initialization. All device communication occurs through page-aligned descriptor rings and polled I/O (no interrupt-driven completion).

## Device Constants

### PCI Identification
```c
#define VIRTIO_BLK_VENDOR_ID    0x1AF4   /* VirtIO vendor ID */
#define VIRTIO_BLK_DEVICE_ID    0x1042   /* VirtIO block device ID */
```

### Sector Configuration
```c
#define VIRTIO_BLK_SECTOR_SIZE  512      /* Sector size in bytes */
```

### Device Config Space Layout
All offsets are from the device config region base address (caps->device_cfg):
```c
#define VIRTIO_BLK_CFG_CAPACITY 0x00     /* u64: capacity in sectors */
```

### Request Header Definition
```c
struct virtio_blk_req {
  uint32_t type;       /* Request type: VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
  uint32_t reserved;   /* Must be zero */
  uint64_t sector;     /* Sector number (512-byte units) */
};
```

### Request Type Constants
```c
#define VIRTIO_BLK_T_IN   0   /* Read request (device → driver) */
#define VIRTIO_BLK_T_OUT  1   /* Write request (driver → device) */
```

### Status Byte Constants
```c
#define VIRTIO_BLK_S_OK     0   /* Request successful */
#define VIRTIO_BLK_S_IOERR  1   /* I/O error */
#define VIRTIO_BLK_S_UNSUPP 2   /* Operation unsupported */
```

## Driver State Structure

```c
struct virtio_blk {
  struct pci_device pci;              /* PCI device info (bus/slot/func/bars) */
  struct virtio_pci_caps pci_caps;    /* VirtIO PCI capability registers (MMIO bases) */
  struct virtqueue vq;                /* Virtqueue 0 state and ring pointers */
  uint64_t capacity_sectors;          /* Device capacity in 512-byte sectors */
};
```

The device maintains a static instance:
```c
static struct virtio_blk blk_dev;
```

### Backing Memory (Page-Aligned, Kernel BSS)
```c
/* Virtqueue descriptor ring: 16 descriptors × 16 bytes = 256 bytes */
static struct virtq_desc blk_desc[VIRTQ_MAX_SIZE] __attribute__((aligned(4096)));

/* Available ring: 2 + 2 + (16 × 2) = 36 bytes (padded to 4096-byte alignment) */
static struct virtq_avail blk_avail __attribute__((aligned(4096)));

/* Used ring: 2 + 2 + (16 × 8) = 132 bytes (padded to 4096-byte alignment) */
static struct virtq_used blk_used __attribute__((aligned(4096)));
```

Where:
- `VIRTQ_MAX_SIZE = 16` (max concurrent requests)
- Each descriptor is 16 bytes (addr:u64, len:u32, flags:u16, next:u16)
- Available ring: flags:u16, idx:u16, ring[16]:u16
- Used ring: flags:u16, idx:u16, ring[16 × used_elem] where used_elem = (id:u32, len:u32)

## Public API

### Initialization

```c
void pci_virtio_blk_init(void);
```

**Purpose**: Initialize the VirtIO block device, discover it on PCI, negotiate features, and set up the virtqueue.

**Preconditions**:
- MMU must be enabled (TTBR0_EL1 and TTBR1_EL1 configured)
- PCI bus enumeration must have completed
- `uart_println()` and logging functions available
- `mmio_read*()` / `mmio_write*()` functions available
- `VIRT_TO_PHYS()` macro available (VA to PA conversion)
- Exception level: EL1 (kernel)

**Postconditions**:
- `blk_dev` struct fully initialized with capacity read
- Virtqueue 0 configured and enabled
- Device status = DRIVER_OK (0x0C)
- Ready to accept read/write requests

**Behavior** (7-step VirtIO initialization sequence):

1. **Device Discovery**
   - Calls `pci_find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID, &blk_dev.pci)`
   - Returns early with error log if device not found

2. **Header Type Validation**
   - Reads PCI header type via `pci_get_header_type()`, masks bits [6:0]
   - Verifies value == PCI_ENDPOINT_DEV_TYPE (0x00)
   - Returns if not an endpoint device

3. **PCI Configuration**
   - Calls `pci_assign_bars()` to map BAR addresses
   - Calls `pci_enable_device()` to enable I/O and memory access
   - Calls `virtio_parse_capabilities()` to extract MMIO base addresses into `blk_dev.pci_caps` (common_cfg, notify_base, isr_cfg, device_cfg, notify_off_multiplier)

4. **Device Reset** (status = 0x00)
   - Write VIRTIO_STATUS_RESET (0) to status register at `base + VIRTIO_COMMON_STATUS`
   - Issue `dsb_sy()` memory barrier
   - Poll status until it equals VIRTIO_STATUS_RESET (busy-wait)

5. **Status Progression**
   - Read current status byte
   - Write status |= VIRTIO_STATUS_ACKNOWLEDGE (1), issue `dsb_sy()`
   - Read current status byte
   - Write status |= VIRTIO_STATUS_DRIVER (2), issue `dsb_sy()`

6. **Feature Negotiation**
   - Set VIRTIO_COMMON_DFSELECT = 0, issue `dsb_sy()`, read VIRTIO_COMMON_DF → feat_lo
   - Set VIRTIO_COMMON_DFSELECT = 1, issue `dsb_sy()`, read VIRTIO_COMMON_DF → feat_hi
   - **Accept only**: guest_lo = 0, guest_hi = feat_hi & 0x01 (VIRTIO_F_VERSION_1 only)
   - Write guest_lo via VIRTIO_COMMON_GFSELECT=0, VIRTIO_COMMON_GF, issue `dsb_sy()`
   - Write guest_hi via VIRTIO_COMMON_GFSELECT=1, VIRTIO_COMMON_GF, issue `dsb_sy()`
   - Set VIRTIO_STATUS_FEATURES_OK (8), issue `dsb_sy()`
   - Read and verify status has VIRTIO_STATUS_FEATURES_OK set; return on failure

7. **Virtqueue Setup & Activation**
   - Call `virtqueue_setup(base, 0, &blk_dev.vq, &blk_dev.pci_caps)`:
     - Writes queue size, PA addresses of desc/avail/used rings
     - Computes notification address from notify_off and notify_off_multiplier
     - Enables queue
   - Return if `virtqueue_setup()` fails
   - Set VIRTIO_STATUS_DRIVER_OK (4), issue `dsb_sy()`
   - Read capacity from device config (u64 at offset 0x00):
     - `mmio_read32(device_cfg + 0) → cap_lo`
     - `mmio_read32(device_cfg + 4) → cap_hi`
     - `capacity_sectors = (cap_hi << 32) | cap_lo`

**Register Access Details** (all via mmio_read/write, with dsb_sy after writes):
- Status register: u8 at offset 0x14 in common config
- Feature select/feature registers: u32 at offsets 0x00/0x04 (device), 0x08/0x0C (guest)
- Device config region: separate MMIO bar, u32-aligned reads at offset 0x00 for capacity

---

### Block I/O Operations

```c
int blk_read(uint64_t sector, void *buf);
```

**Purpose**: Read one 512-byte sector from disk into memory.

**Parameters**:
- `sector`: Sector number (0-based, 512-byte units)
- `buf`: Kernel VA of buffer (must be 512 bytes, naturally aligned)

**Return**:
- ESUCCESS (1) on success
- EERROR (0) on I/O error

**Preconditions**:
- `pci_virtio_blk_init()` must have completed successfully
- `sector < blk_dev.capacity_sectors`
- `buf` is a valid kernel VA (accessible, mapped, not I/O)
- Buffer is not aliased (device won't write during our read of status)

**Behavior**:

1. **Request Construction** (static, persistent)
   ```c
   static struct virtio_blk_req hdr __attribute__((aligned(16)));
   static volatile uint8_t status __attribute__((aligned(16)));
   
   hdr.type = VIRTIO_BLK_T_IN;     /* Read command */
   hdr.reserved = 0;
   hdr.sector = sector;
   status = 0xFF;                   /* Pre-write with sentinel */
   ```

2. **Descriptor Chain Submission** (3 segments):
   - **Segment 0** (header, read-only):
     - PA = VIRT_TO_PHYS(&hdr)
     - Len = sizeof(hdr) (10 bytes: 4 + 4 + 8, padded to 16)
     - Flags = VIRTQ_DESC_F_NONE (device reads)
   - **Segment 1** (data buffer, write):
     - PA = VIRT_TO_PHYS(buf)
     - Len = VIRTIO_BLK_SECTOR_SIZE (512)
     - Flags = VIRTQ_DESC_F_WRITE (device writes)
   - **Segment 2** (status, write):
     - PA = VIRT_TO_PHYS(&status)
     - Len = 1
     - Flags = VIRTQ_DESC_F_WRITE

   Chain is built via `virtqueue_submit_chain(&blk_dev.vq, segs, 3)` which:
   - Links descriptors with VIRTQ_DESC_F_NEXT
   - Updates free_head for next request
   - Publishes to available ring with `dsb_sy()` after avail->idx increment

3. **Device Notification**
   - Call `virtqueue_notify(&blk_dev.vq)`:
     - 16-bit write (value=0) to notify_addr
     - Wakes device to process available queue

4. **Poll for Completion**
   - Call `virtqueue_poll(&blk_dev.vq)`:
     - Busy-wait on used->idx change (up to 10M iterations)
     - Issues `dsb_sy()` after device writes
     - Returns written length from used ring

5. **Status Check**
   - If status != VIRTIO_BLK_S_OK, log error and return EERROR
   - Otherwise return ESUCCESS

**Memory Model**:
- Request header and status are static kernel BSS (persists across calls)
- One request at a time (no concurrent I/O)
- Data buffer contents visible to device via PA mapping (no cache flush required, device is IOMMU-bypass in QEMU)

---

```c
int blk_write(uint64_t sector, const void *buf);
```

**Purpose**: Write one 512-byte sector to disk from memory.

**Parameters**:
- `sector`: Sector number (0-based, 512-byte units)
- `buf`: Kernel VA of buffer (512 bytes, naturally aligned, const)

**Return**:
- ESUCCESS (1) on success
- EERROR (0) on I/O error

**Preconditions**:
- Same as `blk_read()`, except buffer is const (not modified by device)

**Behavior**: Identical to `blk_read()` except:
- `hdr.type = VIRTIO_BLK_T_OUT` (write command)
- **Segment 1** flags = VIRTQ_DESC_F_NONE (device reads buffer, no WRITE flag)

**Blocking Note**: Both `blk_read()` and `blk_write()` block until the device completes the request (no async/interrupt-driven completion in this driver).

---

## VirtIO Protocol Details

### Feature Negotiation

The driver accepts **only** VIRTIO_F_VERSION_1 (bit 32 = feature selector index 1, bit 0). All other device features are rejected. This ensures compatibility with the VirtIO 1.0 spec for structured PCI access and queue layout.

### Virtqueue Descriptor Chain Format

For a 3-segment chain (header → data → status):

```
Descriptor 0 (head):
  addr = PA(hdr)
  len = 10
  flags = 0x0 (no NEXT, device reads)
  next = (unused, not chained in this layout)
  
Descriptor 1:
  addr = PA(buf)
  len = 512
  flags = 0x3 (NEXT | WRITE)
  next = index of descriptor 2
  
Descriptor 2:
  addr = PA(status)
  len = 1
  flags = 0x2 (WRITE, no NEXT)
  next = 0 (end of chain)
```

Available ring entry points to descriptor 0's index; device chains via descriptors.

### MMIO Register Access Pattern

All reads/writes follow this pattern:
```
1. mmio_write*(addr + offset, value) — volatile store
2. dsb_sy() — data synchronization barrier (serializes vs device)
3. (optional) mmio_read*(addr + offset) — volatile load to verify write took effect
```

The `dsb_sy()` barrier ensures:
- Device sees write before next read
- Device writes are visible on next read
- No CPU reordering of I/O operations

### Notification Semantics

The notify register write is always u16 width (as per VirtIO 1.x spec). The device decodes the notify address's offset (relative to notify_base) to determine which queue is being notified. The data value is ignored in polled mode.

```
notify_addr = notify_base + (queue_notify_off[0] * notify_off_multiplier)
mmio_write16(notify_addr, 0)
```

### Polling and Completion Detection

The `virtqueue_poll()` function:
1. Reads `used->idx` (volatile) in a loop, comparing to `last_used`
2. When they differ, device has written `used->ring[last_used % size]`
3. Issues `dsb_sy()` to ensure used ring is fully visible
4. Extracts `used_elem.len` (bytes written by device)
5. Increments `last_used` and returns

For block requests, the device writes:
- Header status field (1 byte written)
- Data payload length in used ring (512 for success)

## Rust Port Strategy

### Module Structure

```rust
pub mod blk {
    pub struct BlkDevice {
        pci: PciDevice,
        pci_caps: VirtioPciCaps,
        vq: Virtqueue,
        capacity_sectors: u64,
    }

    pub static BLK_DEV: Mutex<Option<BlkDevice>> = Mutex::new(None);
    // or use a once_cell::sync::Lazy for single-initialization pattern

    pub fn init() -> Result<(), BlkError>;
    pub fn read(sector: u64, buf: &mut [u8; 512]) -> Result<(), BlkError>;
    pub fn write(sector: u64, buf: &[u8; 512]) -> Result<(), BlkError>;
}
```

### Type Design Recommendations

1. **Device Struct**:
   - Use `NewType` wrappers for physical addresses vs virtual addresses
   - Type-safe `Sector` type (wraps u64) to avoid accidental unit confusion
   - Associated constants for magic numbers (VENDOR_ID, DEVICE_ID, SECTOR_SIZE)

2. **Request Lifecycle**:
   - Create a private `Request` struct encapsulating the descriptor chain
   - Use `#[repr(C, align(16))]` on request header to match C alignment
   - Use volatile cell wrappers or `core::ptr::read_volatile` for status polling

3. **Descriptor Ring Access**:
   - Use `&mut [Descriptor]` slices for safe indexing into descriptor arrays
   - Ensure page alignment via `#[repr(align(4096))]` on backing arrays
   - Consider `PageAligned<[T; N]>` newtype to enforce at compile time

4. **Memory Barriers**:
   - Wrap `dsb_sy()` inline asm in a safe `fn memory_barrier()` in a `barriers` module
   - Call before/after MMIO reads/writes as per C code pattern
   - Clearly document where barriers are required in comments

5. **Error Handling**:
   - Define `enum BlkError { NotFound, FeatureNegotiationFailed, VirtqueueSetupFailed, IoError(u8), Timeout }`
   - Use `Result<T, BlkError>` throughout
   - Map C return codes (ESUCCESS/EERROR) to Rust Result

6. **Static State**:
   - Use `Mutex<Option<BlkDevice>>` for the singleton device if true concurrency exists
   - If no concurrency (single-threaded kernel), use `static mut` with unsafe wrapper or `once_cell::sync::Lazy`
   - Request header and status buffer should be static scratch space (not stack-allocated)

7. **MMIO Access Layer**:
   - Wrap `mmio_read32()`, `mmio_read16()`, `mmio_read8()` and writes in safe functions
   - Ensure volatile semantics are preserved
   - Consider a builder pattern for register reads with type-safe field extraction

### Invariants to Preserve

- **Alignment**: Descriptor ring = 4096B, available ring = 4096B, used ring = 4096B (must be page-aligned for device DMA)
- **Memory Ordering**: Every device register write followed by `dsb_sy()` before subsequent operations
- **Virtqueue Size**: Max 16 descriptors, checked against device capability
- **Feature Bits**: Only accept VIRTIO_F_VERSION_1; reject all others
- **Sector Size**: Hardcoded 512 bytes; reject operations with different sizes
- **Request Timeout**: Poll max ~10M iterations before declaring device wedged

### What Stays in Assembly

- **`dsb_sy()` inline asm**: Data Synchronization Barrier (aarch64-specific); no Rust equivalent in core/std
- **Volatile reads/writes to MMIO**: Use `core::ptr::read_volatile()` and `write_volatile()` for correctness (prevent compiler elision)
- **Feature MSR reads** (if implemented): `mrs`/`msr` instructions for system registers (in virtio_parse_capabilities, called from pci module)

Everything else should be expressible in safe/unsafe Rust.

## Dependencies

### Subsystems Called By blk
- **pci**: `pci_find_device()`, `pci_get_header_type()`, `pci_assign_bars()`, `pci_enable_device()`, `virtio_parse_capabilities()`
- **virtqueue**: `virtqueue_setup()`, `virtqueue_submit_chain()`, `virtqueue_notify()`, `virtqueue_poll()`
- **mmio**: `mmio_read8/16/32()`, `mmio_write8/16/32()`
- **mmu**: `VIRT_TO_PHYS()` macro
- **utils**: `dsb_sy()`, error codes (ESUCCESS/EERROR)
- **uart**: `uart_println()`, `uart_printf()`, `uart_errorln()` for logging (can be stubbed in pure Rust version)

### Subsystems That Call blk
- **kernel**: Calls `pci_virtio_blk_init()` during early_init
- **fat32**: Uses block device for sector I/O (calls `blk_read()`, `blk_write()`)
- **vfs**: Indirectly via FAT32 driver

## Boot/Usage Ordering

1. **Early Boot** (PAS, EL1):
   - MMU enable and upper-half relocation (handles VA → PA mapping)
   - PCI bus enumeration
   - **→ `pci_virtio_blk_init()` called here**
   - Capacity read and device ready

2. **Main Kernel** (upper half, EL1):
   - FAT32 filesystem mounts (uses `blk_read()` to read boot sectors)
   - VFS registers device nodes
   - User tasks can open `/dev/` files backed by block device

3. **User Mode** (EL0):
   - User tasks never call blk directly
   - Kernel syscalls (sys_read, sys_write) on file descriptors trigger block I/O via VFS→FAT32→blk

## Concrete Magic Numbers

| Constant | Value | Source |
|----------|-------|--------|
| VIRTIO_BLK_VENDOR_ID | 0x1AF4 | PCI VirtIO org ID |
| VIRTIO_BLK_DEVICE_ID | 0x1042 | VirtIO block device ID |
| VIRTIO_BLK_SECTOR_SIZE | 512 | Standard disk sector |
| VIRTIO_BLK_T_IN | 0 | Read request opcode |
| VIRTIO_BLK_T_OUT | 1 | Write request opcode |
| VIRTIO_BLK_S_OK | 0 | Success status |
| VIRTIO_BLK_S_IOERR | 1 | I/O error status |
| VIRTIO_BLK_S_UNSUPP | 2 | Unsupported operation status |
| VIRTIO_BLK_CFG_CAPACITY | 0x00 | Device config offset (u64 le) |
| VIRTIO_STATUS_RESET | 0 | Device status value |
| VIRTIO_STATUS_ACKNOWLEDGE | 1 | Device status value |
| VIRTIO_STATUS_DRIVER | 2 | Device status value |
| VIRTIO_STATUS_DRIVER_OK | 4 | Device status value |
| VIRTIO_STATUS_FEATURES_OK | 8 | Device status value |
| VIRTIO_COMMON_DFSELECT | 0x00 | Register offset in common cfg |
| VIRTIO_COMMON_DF | 0x04 | Register offset in common cfg |
| VIRTIO_COMMON_GFSELECT | 0x08 | Register offset in common cfg |
| VIRTIO_COMMON_GF | 0x0C | Register offset in common cfg |
| VIRTIO_COMMON_STATUS | 0x14 | Register offset in common cfg |
| VIRTIO_COMMON_Q_SELECT | 0x16 | Register offset in common cfg |
| VIRTIO_COMMON_Q_SIZE | 0x18 | Register offset in common cfg |
| VIRTIO_COMMON_Q_ENABLE | 0x1C | Register offset in common cfg |
| VIRTIO_COMMON_Q_NOFF | 0x1E | Register offset in common cfg |
| VIRTIO_COMMON_Q_DESCLO | 0x20 | Register offset in common cfg |
| VIRTIO_COMMON_Q_DESCHI | 0x24 | Register offset in common cfg |
| VIRTIO_COMMON_Q_DRIVERLO | 0x28 | Register offset in common cfg |
| VIRTIO_COMMON_Q_DRIVERHI | 0x2C | Register offset in common cfg |
| VIRTIO_COMMON_Q_DEVICELO | 0x30 | Register offset in common cfg |
| VIRTIO_COMMON_Q_DEVICEHI | 0x34 | Register offset in common cfg |
| VIRTIO_COMMON_Q_MSIX | 0x1A | Register offset in common cfg |
| VIRTIO_COMMON_MSIX | 0x10 | Register offset in common cfg |
| VIRTIO_MSI_NO_VECTOR | 0xFFFF | Disable MSI-X (polling mode) |
| VIRTQ_MAX_SIZE | 16 | Max descriptors per queue |
| VIRTQ_DESC_F_NONE | 0 | Descriptor flag: device reads |
| VIRTQ_DESC_F_NEXT | 1 | Descriptor flag: linked chain |
| VIRTQ_DESC_F_WRITE | 2 | Descriptor flag: device writes |
| VIRTIO_F_VERSION_1 | (1 << 32) | Accepted feature (feat_hi bit 0) |
| VIRTQUEUE_POLL_MAX_SPINS | 10000000 | Timeout on poll before wedge |
| PCI_ENDPOINT_DEV_TYPE | 0x00 | PCI header type for endpoint |

---

## Gotchas & Subtle Issues

1. **Memory Barriers**: Every MMIO write must be followed by `dsb_sy()`. Forgetting this causes device to not see writes. Device writes are not visible to CPU without `dsb_sy()` before reading them.

2. **Volatile Access**: Request header and status fields must remain static (or on heap with proper alignment) and be accessed via volatile pointers. Stack-allocated buffers will be optimized away if not marked volatile or if compiler elides writes.

3. **Descriptor Wrapping**: The `free_head` index wraps modulo queue size; descriptors must be linked correctly via the `next` field when building chains. Off-by-one errors cause device to miss segments.

4. **Feature Negotiation**: Must accept features in two 32-bit halves (feature selector). Feature 32 (VERSION_1) is in the high half (selector = 1). Selector must be written and committed (dsb_sy) before reading/writing the feature register.

5. **Page Alignment**: Descriptor rings MUST be 4096-byte aligned for DMA. The static declarations use `__attribute__((aligned(4096)))` in C; Rust must use `#[repr(align(4096))]` structs or similar.

6. **Sector Size Hardcoding**: The driver assumes 512-byte sectors everywhere. The device reports this in its config, but the code doesn't validate it; a device with 4096-byte sectors would silently corrupt data.

7. **Timeout on Poll**: `virtqueue_poll()` spins up to 10M iterations. On a slow emulator or under heavy load, this might timeout prematurely. The grace period is heuristic-based.

8. **Request Serialization**: The request header and status buffers are static and shared across all calls. **Multiple concurrent calls to `blk_read()` / `blk_write()` will corrupt each other's requests.** A Rust port should protect with a Mutex or ensure single-threaded execution.

9. **Status Byte Validity**: The status byte must be checked after `virtqueue_poll()` returns, but there's a race: if the device hasn't written it yet, we might see 0xFF (our sentinel) and incorrectly report success. The code trusts that `used->idx` update is atomic with status write, which is generally safe on cache-coherent systems but technically not guaranteed by the spec.

10. **Capacity U64 Split**: Capacity is read as two 32-bit halves (low then high). On a system where MMIO reads don't guarantee atomicity, there's a small race if the device updates capacity mid-read. Not an issue in practice (capacity is set at boot), but worth noting.

11. **Feature Selection Register Persistence**: The DFSELECT and GFSELECT registers persist across reads. If not explicitly set before each feature read/write, you might read/write the wrong 32-bit half. The code sets them explicitly for clarity.

12. **No Interrupt Support**: This driver polls for completion. If the device is configured with an interrupt (MSI-X vector), we disable it. If the device requires interrupt-based notification, the driver will hang. QEMU in default polled mode works fine.

13. **ABI/Alignment**: The `struct virtio_blk_req` must match the device's expected layout (type:u32, reserved:u32, sector:u64). Different padding or field order breaks communication.

14. **Endianness**: VirtIO uses little-endian for all fields. On a little-endian system (aarch64), this is transparent. Code doesn't explicitly byteswap, assuming LE.

---

## Validation Checklist for Rust Port

- [ ] All constants reproduced exactly (vendor ID, device ID, register offsets, etc.)
- [ ] `struct virtio_blk_req` layout matches C (type:u32, reserved:u32, sector:u64) with `#[repr(C)]`
- [ ] Descriptor rings are 4096-byte aligned
- [ ] `dsb_sy()` called after every MMIO write and before every subsequent read
- [ ] Request header and status are static/persistent (not stack or reallocated)
- [ ] Virtqueue chain building correctly links descriptors with NEXT flag
- [ ] Feature negotiation: only bit 0 of feat_hi accepted (VIRTIO_F_VERSION_1)
- [ ] Polling loop has timeout (10M iterations) to prevent hang
- [ ] Sector read/write buffer sizes are 512 bytes
- [ ] Error codes map correctly (status byte 0 = success, else error)
- [ ] `pci_virtio_blk_init()` called before any `blk_read()`/`blk_write()`
- [ ] Module dependencies (pci, virtqueue, mmio, mmu, utils) are met
- [ ] No unsafe race conditions if blk_read/write are ever called concurrently (use Mutex if needed)
- [ ] Logging (uart_println/printf) can be stubbed or integrated with kernel logger
- [ ] Address space is upper-half (kernel VA with KERNEL_VA_OFFSET)
- [ ] Run integration test: read a sector from FAT32 filesystem and verify content

