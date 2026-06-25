# VirtIO RNG (Random Number Generator) Subsystem Specification

## Overview

The VirtIO RNG subsystem manages a virtio-rng device (vendor ID 0x1AF4, device ID 0x1044) connected via PCI, providing a driver interface for high-quality random bytes from QEMU's entropy source. The subsystem handles PCI enumeration, VirtIO initialization handshake, virtqueue management, and synchronous random byte retrieval through a 256-byte bounce buffer.

## Hardware Constants

### Vendor and Device IDs
- **VIRTIO_RNG_VENDOR_ID**: `0x1AF4` (RedHat/VirtIO vendor)
- **VIRTIO_RNG_DEVICE_ID**: `0x1044` (VirtIO RNG device)

### PCI Header Type
- **PCI_ENDPOINT_DEV_TYPE**: `0x00` — expected header layout

### VirtIO Status Register Values
All in **VIRTIO_COMMON_STATUS** (offset 0x14, u8 RW):
- **VIRTIO_STATUS_RESET**: `0x00` — device reset state
- **VIRTIO_STATUS_ACKNOWLEDGE**: `0x01` — driver acknowledges device
- **VIRTIO_STATUS_DRIVER**: `0x02` — driver initialized
- **VIRTIO_STATUS_FEATURES_OK**: `0x08` — feature negotiation accepted
- **VIRTIO_STATUS_DRIVER_OK**: `0x04` — device ready for operation
- **VIRTIO_STATUS_FAILED**: `0x80` — device malfunction

### VirtIO Common Config Register Offsets (from common_cfg base)
All accesses via mmio_read*/mmio_write* at indicated width:
- **VIRTIO_COMMON_DFSELECT**: `0x00` (u32 RW) — device feature select
- **VIRTIO_COMMON_DF**: `0x04` (u32 R) — device feature (selected by DFSELECT)
- **VIRTIO_COMMON_GFSELECT**: `0x08` (u32 RW) — driver (guest) feature select
- **VIRTIO_COMMON_GF**: `0x0C` (u32 RW) — driver (guest) feature (selected by GFSELECT)
- **VIRTIO_COMMON_MSIX**: `0x10` (u16 RW) — MSI-X config vector
- **VIRTIO_COMMON_NUMQ**: `0x12` (u16 R) — number of queues available
- **VIRTIO_COMMON_STATUS**: `0x14` (u8 RW) — device status
- **VIRTIO_COMMON_CFGGEN**: `0x15` (u8 R) — config generation (for config space changes)
- **VIRTIO_COMMON_Q_SELECT**: `0x16` (u16 RW) — queue select (0 for RNG)
- **VIRTIO_COMMON_Q_SIZE**: `0x18` (u16 RW) — queue size (get max; set negotiated)
- **VIRTIO_COMMON_Q_MSIX**: `0x1A` (u16 RW) — queue MSI-X vector
- **VIRTIO_COMMON_Q_ENABLE**: `0x1C` (u16 RW) — queue enable flag
- **VIRTIO_COMMON_Q_NOFF**: `0x1E` (u16 R) — queue notify offset (for notification address)
- **VIRTIO_COMMON_Q_DESCLO**: `0x20` (u32 RW) — descriptor table address (low 32 bits)
- **VIRTIO_COMMON_Q_DESCHI**: `0x24` (u32 RW) — descriptor table address (high 32 bits)
- **VIRTIO_COMMON_Q_DRIVERLO**: `0x28` (u32 RW) — available ring address (low 32 bits)
- **VIRTIO_COMMON_Q_DRIVERHI**: `0x2C` (u32 RW) — available ring address (high 32 bits)
- **VIRTIO_COMMON_Q_DEVICELO**: `0x30` (u32 RW) — used ring address (low 32 bits)
- **VIRTIO_COMMON_Q_DEVICEHI**: `0x34` (u32 RW) — used ring address (high 32 bits)

### VirtIO Feature Flags
- **VIRTIO_F_VERSION_1**: bit 32 (in feature select index 1, as bit 0) — Version 1.0 compliance (must be negotiated)
- RNG device has no device-specific features; only VIRTIO_F_VERSION_1 is accepted

### VirtQueue Parameters
- **VIRTQ_MAX_SIZE**: `16` — max descriptors per queue
- **VIRTQ_DESC_F_NONE**: `0x0000` — no flags (device reads buffer)
- **VIRTQ_DESC_F_NEXT**: `0x0001` — descriptor continues via `next` field
- **VIRTQ_DESC_F_WRITE**: `0x0002` — device writes to buffer (vs device reads)
- **VIRTIO_MSI_NO_VECTOR**: `0xFFFF` — no MSI-X vector assigned (polling mode)

### RNG Bounce Buffer
- **RNG_BUF_SIZE**: `256` bytes — device-to-driver random buffer capacity

## Struct Layouts

### struct virtio_rng
```c
struct virtio_rng {
  struct pci_device pci;              // PCI device descriptor
  struct virtio_pci_caps pci_caps;    // Parsed VirtIO capability addresses
  struct virtqueue vq;                // Virtqueue 0 (RNG uses only one queue)
};
```

### struct pci_device (used, fields needed)
```c
struct pci_device {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  uint64_t bar_addr[6];    // Base address registers (0-5)
  // ...other fields...
};
```

### struct virtio_pci_caps
```c
struct virtio_pci_caps {
  uintptr_t common_cfg;            // type 1: common configuration MMIO base
  uintptr_t notify_base;           // type 2: notification base (doorbell)
  uintptr_t isr_cfg;               // type 3: ISR status (not used for polling)
  uintptr_t device_cfg;            // type 4: device-specific config (not used for RNG)
  uint32_t notify_off_multiplier;  // multiplier for queue notify offset
};
```

### struct virtqueue
```c
struct virtqueue {
  uint16_t size;              // negotiated queue size (typically 16)
  uint16_t free_head;         // next descriptor index to use
  uint16_t last_used;         // last used.idx we've observed
  uintptr_t notify_addr;      // physical address of notification register
  struct virtq_desc *desc;    // descriptor table (VIRT_TO_PHYS'd PA)
  struct virtq_avail *avail;  // available ring (VIRT_TO_PHYS'd PA)
  struct virtq_used *used;    // used ring (VIRT_TO_PHYS'd PA)
};
```

### struct virtq_desc
```c
struct virtq_desc {
  uint64_t addr;      // physical address of buffer
  uint32_t len;       // buffer length in bytes
  uint16_t flags;     // VIRTQ_DESC_F_* bits
  uint16_t next;      // index of next descriptor (if VIRTQ_DESC_F_NEXT set)
};
```

### struct virtq_avail
```c
struct virtq_avail {
  uint16_t flags;                  // device->driver interrupt suppression
  uint16_t idx;                    // next descriptor index to submit
  uint16_t ring[VIRTQ_MAX_SIZE];   // head indices of submitted descriptors
};
```

### struct virtq_used_elem
```c
struct virtq_used_elem {
  uint32_t id;      // descriptor index written by device
  uint32_t len;     // bytes written by device
};
```

### struct virtq_used
```c
struct virtq_used {
  uint16_t flags;                      // driver->device interrupt suppression
  uint16_t idx;                        // used ring index (incremented by device)
  struct virtq_used_elem ring[VIRTQ_MAX_SIZE];
};
```

## Public API

### void pci_virtio_rng_init(void)

**Signature:**
```c
void pci_virtio_rng_init(void);
```

**Behavior:**
Initializes the VirtIO RNG device via full PCI and VirtIO handshake.

**Steps (in order):**
1. **Find Device**: Call `pci_find_device(VIRTIO_RNG_VENDOR_ID, VIRTIO_RNG_DEVICE_ID, &rng_dev.pci)`. Log "[RNG] Device not found" on failure and return early.
2. **Check Header Type**: Read header type via `pci_get_header_type(&rng_dev.pci)`, mask bits [6:0], verify equals `PCI_ENDPOINT_DEV_TYPE` (0x00). Log "[RNG]: Unexpected header type" and return on mismatch.
3. **Assign BARs**: Call `pci_assign_bars(&rng_dev.pci)` to map PCI memory regions.
4. **Enable Device**: Call `pci_enable_device(&rng_dev.pci)` to enable DMA and MMIO.
5. **Parse Capabilities**: Call `virtio_parse_capabilities(&rng_dev.pci, &rng_dev.pci_caps)` to extract common_cfg, notify_base, notify_off_multiplier from PCI capability space.
6. **VirtIO Init Sequence** (see below).

**VirtIO Initialization (with dsb_sy() after every MMIO write):**

- **Step 1: Reset Device**
  - Write `VIRTIO_STATUS_RESET` (0x00) to common_cfg+VIRTIO_COMMON_STATUS.
  - Emit dsb_sy().
  - Spin until status register reads as VIRTIO_STATUS_RESET (acknowledges reset).
  - Log "[RNG][VIRTIO-INIT][1] Reset Device" then "[RNG][VIRTIO-INIT][1] Reset Device Complete".

- **Step 2: Acknowledge Device**
  - Read status register, OR with VIRTIO_STATUS_ACKNOWLEDGE (0x01), write back.
  - Emit dsb_sy().
  - Log "[RNG][VIRTIO-INIT][2] Ack".

- **Step 3: Set Driver Status**
  - Read status register, OR with VIRTIO_STATUS_DRIVER (0x02), write back.
  - Emit dsb_sy().
  - Log "[RNG][VIRTIO-INIT][3] Driver Status".

- **Step 4: Negotiate Features**
  - Log "[RNG][VIRTIO-INIT][4] Negotiate Features".
  - **Read device features low (index 0):**
    - Write 0 to common_cfg+VIRTIO_COMMON_DFSELECT.
    - Emit dsb_sy().
    - Read common_cfg+VIRTIO_COMMON_DF (u32).
    - Log " Device features[0]: %x".
  - **Read device features high (index 1):**
    - Write 1 to common_cfg+VIRTIO_COMMON_DFSELECT.
    - Emit dsb_sy().
    - Read common_cfg+VIRTIO_COMMON_DF (u32).
    - Log " Device features[1]: %x".
  - **Negotiate guest features:**
    - Guest feature low = 0 (RNG has no device-specific features).
    - Guest feature high = device_feat_hi & 0x01 (accept VIRTIO_F_VERSION_1).
  - **Write guest features:**
    - Write 0 to common_cfg+VIRTIO_COMMON_GFSELECT.
    - Emit dsb_sy().
    - Write guest_lo to common_cfg+VIRTIO_COMMON_GF.
    - Emit dsb_sy().
    - Write 1 to common_cfg+VIRTIO_COMMON_GFSELECT.
    - Emit dsb_sy().
    - Write guest_hi to common_cfg+VIRTIO_COMMON_GF.
    - Emit dsb_sy().
    - Log " Accepted Features: lo=%x hi=%x".

- **Step 5: Set FEATURES_OK**
  - Read status register, OR with VIRTIO_STATUS_FEATURES_OK (0x08), write back.
  - Emit dsb_sy().

- **Step 6: Verify FEATURES_OK**
  - Read status register.
  - Check (status & VIRTIO_STATUS_FEATURES_OK). If clear, log "[RNG] FEATURES_OK failed" and return.
  - Log "[RNG] Status: %x" and "[RNG] FEATURES_OK !".

- **Step 7: Setup Virtqueue 0**
  - Assign statically-allocated page-aligned buffers:
    - `rng_dev.vq.desc = &rng_desc[0]` (page-aligned descriptor table, 16 entries).
    - `rng_dev.vq.avail = &rng_avail` (page-aligned available ring).
    - `rng_dev.vq.used = &rng_used` (page-aligned used ring).
  - Call `virtqueue_setup(common_cfg, 0, &rng_dev.vq, &rng_dev.pci_caps)`.
  - If return != ESUCCESS (1), log "[RNG] Virtqueue setup failed" and return.

- **Step 8: Set DRIVER_OK**
  - Read status register, OR with VIRTIO_STATUS_DRIVER_OK (0x04), write back.
  - Emit dsb_sy().
  - Log "[RNG] DRIVER_OK set".

**Exit:** Return (void).

### int rng_read(void *buf, uint32_t count)

**Signature:**
```c
int rng_read(void *dst, uint32_t count);
```

**Behavior:**
Synchronously read `count` random bytes into `dst`, using the 256-byte bounce buffer and virtqueue submission/polling.

**Algorithm:**
1. Initialize `done = 0`, `out = (uint8_t *)dst`.
2. **Loop while `done < count`:**
   - Calculate `chunk = min(count - done, RNG_BUF_SIZE)` (typically min(remaining, 256)).
   - Convert rng_buf address to physical address: `pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)rng_buf)`.
   - Call `virtqueue_submit(&rng_dev.vq, pa, chunk, VIRTQ_DESC_F_WRITE)` (WRITE flag means device writes to buffer).
   - Call `virtqueue_notify(&rng_dev.vq)` (ring doorbell to notify device).
   - Call `virtqueue_poll(&rng_dev.vq)` (busy-wait for device completion, returns bytes written).
   - Copy bytes from rng_buf into dst: `for (i = 0; i < chunk; i++) out[done + i] = rng_buf[i]`.
   - Increment `done += chunk`.
3. Return (int)done.

**Note:** The caller must ensure count <= INT_MAX and dst is writable.

## Subsystem Data Layout (Static Allocations)

### Page-Aligned Buffers
```c
static struct virtq_desc rng_desc[VIRTQ_MAX_SIZE] __attribute__((aligned(4096)));
// 16 descriptors × 16 bytes = 256 bytes, aligned to 4096-byte page
// Physical address: VIRT_TO_PHYS((uintptr_t)&rng_desc[0])

static struct virtq_avail rng_avail __attribute__((aligned(4096)));
// size: 2 (flags) + 2 (idx) + 2×16 (ring) = 38 bytes, padded to page
// Physical address: VIRT_TO_PHYS((uintptr_t)&rng_avail)

static struct virtq_used rng_used __attribute__((aligned(4096)));
// size: 2 (flags) + 2 (idx) + 8×16 (ring elements) = 132 bytes, padded to page
// Physical address: VIRT_TO_PHYS((uintptr_t)&rng_used)
```

### Bounce Buffer
```c
static uint8_t rng_buf[RNG_BUF_SIZE] __attribute__((aligned(64)));
// 256 bytes, cache-line aligned (64 bytes)
// DMA writes from device land here; copied to caller's buffer byte-by-byte in rng_read()
```

### Global Device State
```c
struct virtio_rng rng_dev;
// Contains pci_device, virtio_pci_caps, and virtqueue (includes desc/avail/used pointers)
```

## Boot/Usage Ordering

1. **Early Boot** (in kernel_main):
   - PMM initialized (physical memory).
   - MMU initialized (virtual memory mapping, VIRT_TO_PHYS defined).
   - PCI bus enumeration (pci_enumerate_bus()) called.
   - `pci_virtio_rng_init()` called (typically after PCI enumeration, before VFS).

2. **At Initialization**:
   - rng_dev is populated with PCI device info, VirtIO capability addresses, and virtqueue descriptors.
   - Static buffers (rng_desc, rng_avail, rng_used, rng_buf) are zeroed and initialized to empty/idle state.

3. **At Runtime**:
   - Userspace or kernel code calls `rng_read(buf, count)` (typically through /dev/rng device file).
   - Each call submits one or more virtqueue requests, polls for completion, copies bytes out.
   - Multiple concurrent rng_read() calls are NOT thread-safe in the C version (single static virtqueue).

## MMIO and Memory Barrier Requirements

### MMIO Access Constraints
- All VirtIO common config register reads and writes must use the correct width (u8, u16, u32).
- After every register write affecting device state (status, queue addresses, etc.), emit **dsb_sy()** (Data Synchronization Barrier, full system).
- Reads and writes are not reordered by the ARM MMU within a device region, but dsb_sy() ensures prior DMA writes to descriptor memory have been observed.

### DMA Memory Ordering
- After updating virtq_avail->idx (to add a descriptor to the available ring), emit dsb_sy() before ringing the notify doorbell. This ensures the descriptor and avail ring update are visible to the device before notification.
- When reading virtq_used->idx in virtqueue_poll(), emit dsb_sy() after observing the update to ensure any subsequent descriptor reads see the device's updates.

### Physical Address Conversion
- Kernel virtual addresses (VA) are converted to physical addresses (PA) via `VIRT_TO_PHYS(va) = va - KERNEL_VA_OFFSET`.
- Only physical addresses are written to MMIO registers for DMA buffers (descriptor table, available ring, used ring).

## Non-Threadsafe Aspects & Future Locking Considerations

- **Global State:** rng_dev, rng_desc, rng_avail, rng_used, rng_buf are all static global.
- **Virtqueue Free Head:** rng_dev.vq.free_head is modified by virtqueue_submit() without locking.
- **Concurrent Calls:** Two concurrent rng_read() calls will race on free_head, descriptor table writes, and avail->idx updates, leading to descriptor collisions or dropped requests.
- **Polling:** virtqueue_poll() spins up to 10 million iterations; no timeout mechanism.

For multi-threaded usage, a mutex or spinlock protecting rng_dev and the virtqueue state is required.

## Hardware Interaction Sequence (rng_read example)

```
[User] rng_read(buf, 64)
  |
  +-- chunk = 64
  |   pa = VIRT_TO_PHYS(&rng_buf[0])
  |   
  |   virtqueue_submit(&vq, pa, 64, VIRTQ_DESC_F_WRITE):
  |     desc[0] = { addr=pa, len=64, flags=VIRTQ_DESC_F_WRITE, next=0 }
  |     avail->ring[0] = 0
  |     avail->idx = 1
  |     dsb_sy()
  |   
  |   virtqueue_notify(&vq):
  |     mmio_write16(notify_addr, 0)
  |   
  |   [Device] receives notification
  |   [Device] reads desc[0].addr, writes 64 random bytes
  |   [Device] updates used->ring[0] = {id=0, len=64}
  |   [Device] increments used->idx to 1
  |
  |   virtqueue_poll(&vq):
  |     spins until used->idx > last_used (0)
  |     reads len = used->ring[0].len (64)
  |     last_used++ => 1
  |     returns 64
  |
  |   copy 64 bytes from rng_buf to user's buf
  |   done += 64
  |
  +-- return (int)64
[User] gets 64 random bytes
```

## Rust Porting Strategy

### Module Structure
- Create `src/kernel/rng/mod.rs` with core virtio-rng logic.
- Sub-modules or inline:
  - `rng_dev: RngDevice` — static mut global holding PCI device, virtqueue, and capability info.
  - `rng_buf: [u8; 256]` — bounce buffer, properly aligned.
  - `rng_desc, rng_avail, rng_used` — statically allocated page-aligned ring buffers.

### Types
```rust
pub struct RngDevice {
    pci: PciDevice,
    caps: VirtioPciCaps,
    vq: Virtqueue,
}

// Static instance
static mut RNG_DEV: Option<RngDevice> = None;

// Page-aligned buffers (use #[repr(align(4096))])
#[repr(align(4096))]
struct RngDescriptorTable {
    descs: [VirtqDesc; 16],
}

#[repr(align(4096))]
struct RngAvailRing {
    avail: VirtqAvail,
}

#[repr(align(4096))]
struct RngUsedRing {
    used: VirtqUsed,
}

#[repr(align(64))]
struct RngBounceBuffer {
    buf: [u8; 256],
}
```

### Ownership & Safety
- `RngDevice` is initialized once at boot, never moved or deallocated.
- Use `unsafe` blocks for static mut access, guarded by single-threaded init-only or future spinlock.
- `Virtqueue` methods that mutate free_head, avail->idx, etc., require &mut self or interior mutability (Cell/RefCell, but not in kernel context).
- MMIO reads/writes remain in separate `mmio` module; import as `mmio::{read8, write8, read16, write16, read32, write32}`.

### Key Functions to Port
- `rng_init()` — replaces `pci_virtio_rng_init()`. Public, no return; logs errors via uart/logging.
- `rng_read(buf: &mut [u8]) -> usize` — replaces `int rng_read(void *dst, uint32_t count)`. Returns bytes read.

### Locking
- **Single-threaded boot:** No synchronization needed during init.
- **Multi-threaded runtime:** Add a `SpinLock<RngState>` or `Mutex<RngState>` around virtqueue mutations to protect concurrent rng_read() calls. The spec notes this is currently unsafe; Rust's type system enforces the fix.

### Feature Flags & Dependencies
- Link `core`, `core::mem`, `core::sync::atomic` for volatile operations.
- Import from `pci` subsystem: `PciDevice`, `pci_find_device()`, `pci_get_header_type()`, etc.
- Import from `virtio` subsystem: `VirtioPciCaps`, `virtio_parse_capabilities()`.
- Import from `virtqueue` subsystem: `Virtqueue`, `virtqueue_setup()`, `virtqueue_submit()`, `virtqueue_notify()`, `virtqueue_poll()`.
- Import from `mmio` subsystem: `mmio_read8()`, `mmio_write8()`, etc.
- Import from `mmu` subsystem: `VIRT_TO_PHYS` macro or function.
- Logging: `uart_println!()`, `uart_printf!()`, or structured logger.

### Barriers & Volatile Access
- Wrap all MMIO reads/writes as volatile to prevent compiler optimization.
- After every register write, call a `dsb_sy()` wrapper (likely in `arch::sync` or `asm!("dsb sy")`).
- Virtqueue avail and used rings contain fields updated by hardware; mark as volatile or use `core::sync::atomic` for the idx fields.

### Testing Strategy
1. Verify pci_virtio_rng_init() completes without error.
2. Call rng_read() with small buffer, verify returned bytes differ between calls (entropy check).
3. Call rng_read() with large buffer (>256 bytes) to exercise multi-chunk loop.
4. Once userspace is ready, test via `/dev/rng` device file (reads go through vfs → device node → rng_read()).

---

## Summary of Exact Constants and Register Offsets

| Symbol | Value | Width | RW | Purpose |
|--------|-------|-------|----|----|
| VIRTIO_RNG_VENDOR_ID | 0x1AF4 | — | R | VirtIO vendor ID |
| VIRTIO_RNG_DEVICE_ID | 0x1044 | — | R | VirtIO RNG device ID |
| PCI_ENDPOINT_DEV_TYPE | 0x00 | — | — | PCI header type for endpoint |
| VIRTIO_STATUS_RESET | 0x00 | u8 | — | Device reset value |
| VIRTIO_STATUS_ACKNOWLEDGE | 0x01 | u8 | — | Driver ACK bit |
| VIRTIO_STATUS_DRIVER | 0x02 | u8 | — | Driver init bit |
| VIRTIO_STATUS_FEATURES_OK | 0x08 | u8 | — | Feature negotiation done |
| VIRTIO_STATUS_DRIVER_OK | 0x04 | u8 | — | Device ready bit |
| VIRTIO_STATUS_FAILED | 0x80 | u8 | — | Device error bit |
| VIRTIO_COMMON_DFSELECT | 0x00 | u32 | RW | Feature select |
| VIRTIO_COMMON_DF | 0x04 | u32 | R | Feature value |
| VIRTIO_COMMON_GFSELECT | 0x08 | u32 | RW | Guest feature select |
| VIRTIO_COMMON_GF | 0x0C | u32 | RW | Guest feature value |
| VIRTIO_COMMON_MSIX | 0x10 | u16 | RW | MSI-X vector |
| VIRTIO_COMMON_NUMQ | 0x12 | u16 | R | Queue count |
| VIRTIO_COMMON_STATUS | 0x14 | u8 | RW | Device status |
| VIRTIO_COMMON_CFGGEN | 0x15 | u8 | R | Config generation |
| VIRTIO_COMMON_Q_SELECT | 0x16 | u16 | RW | Queue select |
| VIRTIO_COMMON_Q_SIZE | 0x18 | u16 | RW | Queue size |
| VIRTIO_COMMON_Q_MSIX | 0x1A | u16 | RW | Queue MSI-X |
| VIRTIO_COMMON_Q_ENABLE | 0x1C | u16 | RW | Queue enable |
| VIRTIO_COMMON_Q_NOFF | 0x1E | u16 | R | Notify offset |
| VIRTIO_COMMON_Q_DESCLO | 0x20 | u32 | RW | Desc addr low |
| VIRTIO_COMMON_Q_DESCHI | 0x24 | u32 | RW | Desc addr high |
| VIRTIO_COMMON_Q_DRIVERLO | 0x28 | u32 | RW | Avail addr low |
| VIRTIO_COMMON_Q_DRIVERHI | 0x2C | u32 | RW | Avail addr high |
| VIRTIO_COMMON_Q_DEVICELO | 0x30 | u32 | RW | Used addr low |
| VIRTIO_COMMON_Q_DEVICEHI | 0x34 | u32 | RW | Used addr high |
| VIRTQ_DESC_F_NONE | 0x0000 | — | — | Device reads |
| VIRTQ_DESC_F_NEXT | 0x0001 | — | — | Chained descriptor |
| VIRTQ_DESC_F_WRITE | 0x0002 | — | — | Device writes |
| VIRTIO_MSI_NO_VECTOR | 0xFFFF | — | — | Polling (no interrupt) |
| VIRTQ_MAX_SIZE | 16 | — | — | Max descriptors |
| RNG_BUF_SIZE | 256 | — | — | Bounce buffer size |
| VIRTIO_F_VERSION_1 | bit 32 (1 in feat[1]) | — | — | Version 1.0 compliance |

