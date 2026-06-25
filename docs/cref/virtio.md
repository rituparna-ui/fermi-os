# VirtIO PCI Transport Layer - Rust Porting Specification

## Overview

The VirtIO subsystem provides a high-level interface for communicating with emulated devices (QEMU virt machine) via the PCI bus. This port covers:

1. **PCI Capability Walking**: Parse vendor-specific capabilities to locate four memory-mapped config regions
2. **Common Config MMIO**: Device feature negotiation, queue setup, device status lifecycle
3. **Virtqueue Split Ring**: Descriptor table, available ring (driver→device), used ring (device→driver) 
4. **Notification & Polling**: Ring doorbells and busy-wait completion detection

The subsystem is **transport-only** — device-specific logic (RNG, block, network) lives in separate drivers that call this API. This spec covers the core mechanics that all VirtIO devices share.

## Hardware Constants & Bitfields

### Device Status Register

Location: `VIRTIO_COMMON_STATUS` (offset 0x14 in common config)

```
VIRTIO_STATUS_RESET      = 0x00  ; Device not running
VIRTIO_STATUS_ACKNOWLEDGE = 0x01 ; Driver acknowledged device
VIRTIO_STATUS_DRIVER     = 0x02  ; Driver claiming responsibility
VIRTIO_STATUS_DRIVER_OK  = 0x04  ; Device live, negotiation complete
VIRTIO_STATUS_FEATURES_OK = 0x08 ; Feature negotiation succeeded
VIRTIO_STATUS_FAILED     = 0x80  ; Device encountered fatal error
```

### Common Config Register Offsets

Base address: physical address from PCI capability, then mapped at kernel VA = PA + KERNEL_VA_OFFSET (0xFFFF000000000000).

All offsets relative to `common_cfg_base`:

```
0x00  VIRTIO_COMMON_DFSELECT      u32 rw  ; Device feature selector (0=lo, 1=hi)
0x04  VIRTIO_COMMON_DF            u32 r   ; Device features (selected by DFSELECT)
0x08  VIRTIO_COMMON_GFSELECT      u32 rw  ; Guest (driver) feature selector
0x0C  VIRTIO_COMMON_GF            u32 rw  ; Guest features (selected by GFSELECT)
0x10  VIRTIO_COMMON_MSIX          u16 rw  ; MSI-X config vector (0xFFFF=polling)
0x12  VIRTIO_COMMON_NUMQ          u16 r   ; Number of available queues
0x14  VIRTIO_COMMON_STATUS        u8  rw  ; Device status
0x15  VIRTIO_COMMON_CFGGEN        u8  r   ; Config generation counter
0x16  VIRTIO_COMMON_Q_SELECT      u16 rw  ; Queue selector
0x18  VIRTIO_COMMON_Q_SIZE        u16 rw  ; Queue size (read/set)
0x1A  VIRTIO_COMMON_Q_MSIX        u16 rw  ; Queue MSI-X vector (0xFFFF=polling)
0x1C  VIRTIO_COMMON_Q_ENABLE      u16 rw  ; Queue enabled (1=yes)
0x1E  VIRTIO_COMMON_Q_NOFF        u16 r   ; Queue notify offset (in units of notify_off_multiplier)
0x20  VIRTIO_COMMON_Q_DESCLO      u32 rw  ; Descriptor table physical address (low 32 bits)
0x24  VIRTIO_COMMON_Q_DESCHI      u32 rw  ; Descriptor table physical address (high 32 bits)
0x28  VIRTIO_COMMON_Q_DRIVERLO    u32 rw  ; Available ring physical address (low 32 bits)
0x2C  VIRTIO_COMMON_Q_DRIVERHI    u32 rw  ; Available ring physical address (high 32 bits)
0x30  VIRTIO_COMMON_Q_DEVICELO    u32 rw  ; Used ring physical address (low 32 bits)
0x34  VIRTIO_COMMON_Q_DEVICEHI    u32 rw  ; Used ring physical address (high 32 bits)
```

### PCI Capability Parsing

**Capability ID**: 0x09 (Vendor Specific)

**Capability Header Layout** (at config space offset `cap_ptr`):

```
cap_ptr + 0x00: cap_vndr        u8   ; Capability vendor ID (0x09)
cap_ptr + 0x01: cap_next        u8   ; Next capability offset (or 0)
cap_ptr + 0x02: cap_len         u8   ; Capability length
cap_ptr + 0x03: cfg_type        u8   ; Configuration type:
                                        1 = COMMON_CFG
                                        2 = NOTIFY_CFG
                                        3 = ISR_CFG
                                        4 = DEVICE_CFG
                                        5 = PCI_CFG (not needed with ECAM)
cap_ptr + 0x04: bar             u8   ; PCI BAR index (0-5)
cap_ptr + 0x05: pad             [3]u8; Padding
cap_ptr + 0x08: offset          u32  ; Offset within BAR
cap_ptr + 0x0C: length          u32  ; Size in bytes
```

**For NOTIFY_CFG only**, the capability has an additional field:

```
cap_ptr + 0x10: notify_off_multiplier u32 ; Multiplier for queue notify offset
```

### Virtqueue Split Ring Structures

**Descriptor (16 bytes, naturally aligned)**

```
struct virtq_desc {
  uint64_t addr;      ; Physical address of buffer
  uint32_t len;       ; Buffer length in bytes
  uint16_t flags;     ; Descriptor flags (see below)
  uint16_t next;      ; Index of next descriptor in chain (if VIRTQ_DESC_F_NEXT set)
};
```

**Descriptor Flags:**

```
VIRTQ_DESC_F_NONE  = 0x0  ; No flags (device reads buffer)
VIRTQ_DESC_F_NEXT  = 0x1  ; This descriptor continues the chain
VIRTQ_DESC_F_WRITE = 0x2  ; Device writes into this buffer (vs reading from it)
```

**Available Ring Header (4 bytes + array)**

```
struct virtq_avail {
  uint16_t flags;              ; Interrupt suppression flags
  uint16_t idx;                ; Index of next free slot in ring[]
  uint16_t ring[size];         ; Descriptor indices (published by driver)
};
```

Must be allocated with natural alignment (4 bytes, i.e., offset 0 mod 2).

**Used Ring Element (8 bytes each)**

```
struct virtq_used_elem {
  uint32_t id;                 ; Descriptor index that generated this completion
  uint32_t len;                ; Number of bytes written by device
};
```

**Used Ring Header (4 bytes + array)**

```
struct virtq_used {
  uint16_t flags;              ; Interrupt suppression flags
  uint16_t idx;                ; Index of next write slot in ring[]
  virtq_used_elem ring[size];  ; Completion descriptors (written by device)
};
```

Must be allocated with natural alignment (4 bytes, i.e., offset 0 mod 2).

### Virtqueue Sizes & Limits

```
VIRTQ_MAX_SIZE = 16              ; Maximum descriptors per queue
VIRTIO_MSI_NO_VECTOR = 0xFFFF    ; Disable interrupts; use polling instead
```

### Feature Bits

Device and guest (driver) features are negotiated in two 32-bit halves selected by DFSELECT/GFSELECT:

```
Feature set 0 (selector 0): Device-specific and common low bits
Feature set 1 (selector 1): 
  Bit 0 (global bit 32): VIRTIO_F_VERSION_1 = 0x01
  Bit 1 (global bit 33): VIRTIO_F_ACCESS_PLATFORM = 0x02
  ... others depend on device
```

For RNG device: no device-specific features; driver accepts only VIRTIO_F_VERSION_1.

## Public API

### Capability Walking & Config Discovery

#### `virtio_parse_capabilities(dev, caps)`

**Signature:**
```c
void virtio_parse_capabilities(struct pci_device *dev,
                               struct virtio_pci_caps *caps);
```

**Behavior:**
- Walks the PCI capability list starting at `pci_config_read8(dev, PCI_CAP_PTR)`
- For each capability with ID 0x09 (vendor-specific), calls `virtio_populate_capabilities()`
- Fills in the four config region addresses in `caps`:
  - `common_cfg`: Physical address of COMMON_CFG region
  - `notify_base`: Physical address of NOTIFY_CFG region base
  - `notify_off_multiplier`: Multiplier for queue offset calculation
  - `isr_cfg`: Physical address of ISR status region
  - `device_cfg`: Physical address of device-specific config region

**Error handling:**
- If BAR index >= 6, logs error and skips capability
- If capabilities bit not set in PCI status, returns early with error logged
- No exceptions/panics; errors logged to UART

### Virtqueue Lifecycle

#### `virtqueue_setup(common_cfg_base, queue_idx, vq, caps)`

**Signature:**
```c
int virtqueue_setup(uintptr_t common_cfg_base, uint16_t queue_idx,
                    struct virtqueue *vq, struct virtio_pci_caps *caps);
```

**Preconditions:**
- `vq->desc`, `vq->avail`, `vq->used` pointers must be pre-initialized by caller
  - Each must point to page-aligned kernel VA (will be converted to PA via VIRT_TO_PHYS)
  - Descriptor table: 16 bytes × queue size (typically 16 descriptors)
  - Available ring: 4 + 2×size bytes
  - Used ring: 4 + 8×size bytes
- Device must have been through reset and acknowledge steps (status bits)
- `caps` must be populated from `virtio_parse_capabilities()`

**Behavior:**
1. Disable MSI-X for config space (polling mode): write 0xFFFF to VIRTIO_COMMON_MSIX
2. Issue DSB SY memory barrier
3. Select queue by writing queue_idx to VIRTIO_COMMON_Q_SELECT
4. Read max queue size from VIRTIO_COMMON_Q_SIZE
5. Use min(VIRTQ_MAX_SIZE, max_size) as negotiated size
6. Zero-initialize descriptor, available, and used ring structures
7. Write physical addresses (as two 32-bit halves) to device:
   - Descriptor table PA → VIRTIO_COMMON_Q_DESCLO/HI
   - Available ring PA → VIRTIO_COMMON_Q_DRIVERLO/HI
   - Used ring PA → VIRTIO_COMMON_Q_DEVICELO/HI
8. Issue DSB SY
9. Read notify offset from VIRTIO_COMMON_Q_NOFF
10. Compute `notify_addr = caps->notify_base + (notify_off × notify_off_multiplier)`
11. Initialize `vq->size`, `vq->free_head = 0`, `vq->last_used = 0`
12. Enable queue by writing 1 to VIRTIO_COMMON_Q_ENABLE
13. Issue DSB SY

**Return:**
- ESUCCESS (1) if setup succeeded
- EERROR (0) if max_size is 0 (queue unavailable)

**Memory Barriers:**
- DSB SY after each group of MMIO writes to ensure ordering
- Volatile semantics for ring accesses (via volatile pointers)

#### `virtqueue_submit(vq, buf_pa, len, flags)`

**Signature:**
```c
void virtqueue_submit(struct virtqueue *vq, uint64_t buf_pa, uint32_t len,
                      uint16_t flags);
```

**Behavior:**
- Single-segment submission for simple request
- Get next free descriptor index from `vq->free_head`
- Fill descriptor: addr=buf_pa, len, flags, next=0
- Increment free_head (with wrap: `(idx+1) % vq->size`)
- Write descriptor index to available ring at `avail->ring[avail->idx % size]`
- Issue DSB SY
- Increment `avail->idx` (no wrap on this counter; device sees low bits)
- Issue DSB SY
- Does NOT ring doorbell (caller does via `virtqueue_notify()`)

#### `virtqueue_notify(vq)`

**Signature:**
```c
void virtqueue_notify(struct virtqueue *vq);
```

**Behavior:**
- Write 16-bit value 0 to `vq->notify_addr`
- Virtio spec 1.x §4.1.4.4: mandates 16-bit write (even though value is unused)
- Device uses notify_addr's position to look up queue; data value is ignored

#### `virtqueue_poll(vq)`

**Signature:**
```c
uint32_t virtqueue_poll(struct virtqueue *vq);
```

**Behavior:**
- Busy-wait (spin loop) until device advances `used->idx` beyond `vq->last_used`
- Timeout: spin at most 10,000,000 iterations (multi-millisecond grace period before timeout)
- On timeout, log error and return 0
- Once used->idx advances:
  - Issue DSB SY
  - Read used ring at index `last_used % size` to get completion element
  - Extract length field from completion element
  - Increment `last_used` (wraps at 2^16)
  - Return bytes written by device
- Spins on volatile read of `used->idx`

#### `virtqueue_submit_chain(vq, segs, n)`

**Signature:**
```c
uint16_t virtqueue_submit_chain(struct virtqueue *vq,
                                const struct virtq_seg *segs, uint16_t n);
```

**Input struct (caller-provided array):**

```c
struct virtq_seg {
  uint64_t pa;        ; Physical address of segment buffer
  uint32_t len;       ; Segment length
  uint16_t flags;     ; Descriptor flags (VIRTQ_DESC_F_WRITE if device-to-driver)
};
```

**Behavior:**
- Link N segments into a descriptor chain
- Head index = `vq->free_head`
- For each segment i in [0, n):
  - Descriptor index = `(head + i) % vq->size`
  - Copy segment data to descriptor
  - If i < n-1: set VIRTQ_DESC_F_NEXT flag and link next = (head + i + 1) % size
  - If i == n-1: next = 0 (end of chain)
- Update `vq->free_head = (head + n) % vq->size`
- Write chain head index to available ring
- Increment `avail->idx`
- Issue DSB SY after each group of writes
- Return head descriptor index

## Device Initialization Sequence

The RNG driver demonstrates typical initialization flow. All drivers follow this pattern:

### Phase 1: PCI Discovery & Config

1. Call `pci_find_device(vendor_id, device_id, &pci_dev)` to locate device
2. Verify header type is endpoint (bit [6:0] == 0x00)
3. Call `pci_assign_bars(&pci_dev)` to map BAR addresses
4. Call `pci_enable_device(&pci_dev)` to set memory and bus master bits
5. Call `virtio_parse_capabilities(&pci_dev, &caps)` to extract config addresses

### Phase 2: Device Status

All status writes include DSB SY barrier:

1. **Reset**: Write VIRTIO_STATUS_RESET to status register, wait for complete
2. **Acknowledge**: Read status, write with VIRTIO_STATUS_ACKNOWLEDGE bit set
3. **Driver**: Read status, write with VIRTIO_STATUS_DRIVER bit set
4. **Feature negotiation** (see below)
5. **FEATURES_OK**: Write with VIRTIO_STATUS_FEATURES_OK bit, then re-read to verify device accepted
6. **Queue setup**: Call `virtqueue_setup()` for each queue
7. **DRIVER_OK**: Write with VIRTIO_STATUS_DRIVER_OK bit

### Phase 3: Feature Negotiation

1. Read device feature low: write 0 to DFSELECT, read DF
2. Read device feature high: write 1 to DFSELECT, read DF
3. Compute guest features (subset of device features):
   - For RNG: accept bit 0 of feature set 1 (VIRTIO_F_VERSION_1)
   - For other devices: device-specific bits
4. Write guest feature low: write 0 to GFSELECT, write GF
5. Write guest feature high: write 1 to GFSELECT, write GF

## Memory Layout & VA/PA Translation

### Virtual Address Space

- **Kernel upper half**: VA range 0xFFFF000000000000 – 0xFFFFFFFFFFFFFFFF
- **KERNEL_VA_OFFSET**: 0xFFFF000000000000
- **Translation**: PA ↔ VA via VIRT_TO_PHYS(va) = va - KERNEL_VA_OFFSET; PHYS_TO_VIRT(pa) = pa + KERNEL_VA_OFFSET

### MMIO Access

MMIO reads/writes go through these functions (from lib/mmio/mmio.h):

```c
void mmio_write32(uintptr_t addr, uint32_t value);
uint32_t mmio_read32(uintptr_t addr);
void mmio_write16(uintptr_t addr, uint16_t value);
uint16_t mmio_read16(uintptr_t addr);
void mmio_write8(uintptr_t addr, uint8_t value);
uint8_t mmio_read8(uintptr_t addr);
```

- `addr` is treated as a **physical address**
- MMIO layer adds `mmio_va_offset` (set to KERNEL_VA_OFFSET after `mmio_switch_to_upper()`)
- All access is volatile (compiler won't optimize away reads/writes)

### Virtqueue Ring Allocation

Each ring must be **page-aligned** (4 KiB boundary). In the C code:

```c
static struct virtq_desc rng_desc[VIRTQ_MAX_SIZE] __attribute__((aligned(4096)));
static struct virtq_avail rng_avail __attribute__((aligned(4096)));
static struct virtq_used rng_used __attribute__((aligned(4096)));
```

Linker ensures these symbols are page-aligned. Drivers store kernel VA pointers; `virtqueue_setup()` converts to PA.

### DMA Buffer Alignment

Bounce buffers (e.g., RNG output) should be 64-byte aligned for cache efficiency:

```c
static uint8_t rng_buf[256] __attribute__((aligned(64)));
```

## Data Synchronization & Memory Barriers

### DSB SY Semantics

`dsb_sy()` is a full system data synchronization barrier:

```c
void dsb_sy() {
  __asm__ volatile("dsb sy" ::: "memory");
}
```

Ensures:
- All preceding memory operations are visible globally before any succeeding operation
- Compiler cannot reorder loads/stores across the barrier
- Essential for MMIO: a write followed by DSB SY ensures the device sees the write before the next instruction

### Usage Pattern in Virtqueue Code

**After config writes**: DSB SY after each mmio_write* to guarantee device observes changes before next register access.

**Before reading result**: DSB SY after detecting a device change (e.g., used->idx advance) before reading the result to ensure the device has finished writing.

**Volatile semantics**: Used ring reads use volatile pointers to prevent the compiler from caching values:

```c
while (*(volatile uint16_t *)&vq->used->idx == vq->last_used) {
  // spin
}
```

## Error Handling & Failure Modes

### Soft Failures

Errors are logged to UART but do not panic:
- Invalid BAR index in capability → log error, skip capability
- Capabilities not present in PCI status → log error, return
- Queue size = 0 → return EERROR from virtqueue_setup
- Unknown capability type → log, continue

### Timeouts

`virtqueue_poll()` has a 10M iteration timeout to prevent indefinite hangs if device is wedged.

## Rust Module Structure

### High-Level Module Hierarchy

```
crate::virtio
├── mod.rs                    // Capability walking, public exports
├── virtqueue.rs              // Split ring submit/notify/poll
├── cap_iter.rs               // Capability iteration helper
└── transport/                // Transport-specific
    ├── pci.rs                // PCI config space access wrapper
    └── mmio.rs               // MMIO read/write abstractions
```

### Key Struct Design

**VirtioDevice** (generic transport-independent):

```rust
pub struct VirtioDevice {
    common_cfg: PhysicalAddress,
    notify_base: PhysicalAddress,
    notify_off_multiplier: u32,
    isr_cfg: PhysicalAddress,
    device_cfg: PhysicalAddress,
}
```

**Virtqueue** (generic):

```rust
pub struct Virtqueue {
    size: u16,
    free_head: u16,
    last_used: u16,
    
    notify_addr: PhysicalAddress,
    
    desc: &'static mut [VirtqDesc],
    avail: &'static mut VirtqAvail,
    used: &'static mut VirtqUsed,
}
```

### Static/Storage

- Virtqueue rings must be statics (page-aligned, long-lived), not heap-allocated
- Each driver (RNG, block, net) owns its queue ring statics
- Rings are typically initialized to `MaybeUninit` at compile time, then filled during device init

### Locking Strategy

Single-threaded kernel (no preemption at virtio layer). Virtio is initialized before scheduler. Device-specific drivers manage any per-device state that might be accessed from multiple contexts.

### What Must Stay Assembly

**Nothing** in the virtio transport layer itself requires assembly:
- All MMIO access can be inline asm or C functions (already done via mmio_read/write)
- DSB SY barrier is inline asm but already abstracted to `dsb_sy()` function

**Note**: Virtio *drivers* (e.g., RNG, block, network) may need assembly for driver-specific needs (e.g., network driver might have interrupt handlers), but the core virtio transport does not.

## Concurrency & Interrupt Considerations

The current C implementation:
- Runs single-threaded at boot
- Uses polling (`virtqueue_poll()`) instead of interrupts
- Sets MSI-X vector to 0xFFFF (polling mode) in both config and per-queue regs
- No synchronization primitives (no locks, atomics, or volatile accesses except on ring indices)

**Rust port implications:**
- Single-threaded phase: no Mutex/Arc needed
- If future work adds interrupts: ISR would need volatile atomics for `used->idx` and careful barrier placement
- For now: straightforward translation with volatile for user ring (already in C code)

## Constants Summary

**PCI/VirtIO identifiers:**
```
VIRTIO_RNG_VENDOR_ID  = 0x1AF4
VIRTIO_RNG_DEVICE_ID  = 0x1044
```

**Feature bit (RNG only):**
```
VIRTIO_F_VERSION_1    = 1 << 32  (bit 0 of feature set 1)
```

**Config space constants:**
```
PCI_CAP_PTR           = 0x34
PCI_STATUS            = 0x06
```

**Virtual address mapping:**
```
KERNEL_VA_OFFSET      = 0xFFFF000000000000
```

**MMIO layout (QEMU virt machine):**
```
PCI_ECAM_PHYS         = 0x4010000000 (256 buses)
PCI_MMIO32_PHYS       = 0x10000000
PCI_MMIO32_LIMIT      = 0x3EFEFFFFUL
PCI_MMIO64_PHYS       = 0x8000000000
PCI_MMIO64_LIMIT      = 0xFFFFFFFFFFUL
```

## Testing & Verification

The C RNG driver can be verified to work end-to-end:
1. Device is found at enumeration
2. Capabilities are parsed
3. Status transitions complete (no errors logged)
4. Virtqueue setup succeeds
5. RNG reads return random bytes via `rng_read()`

Rust port should demonstrate equivalent behavior.

## Notable Gotchas & Subtle Issues

1. **Queue index wrapping**: `avail->idx` and `used->idx` are u16 that wrap at 2^16; comparison logic must account for this (currently done via `(last_used % size)` indexing, which is safe up to 65k calls).

2. **Physical address split**: 64-bit addresses must be split into two 32-bit writes to common config (DESCLO/HI, DRIVERLO/HI, DEVICELO/HI). No atomic 64-bit MMIO write available on ARM64.

3. **Notify multiplier application**: Queue notify address is NOT `notify_base + queue_idx` but rather `notify_base + (notify_off * multiplier)`. The offset comes from reading VIRTIO_COMMON_Q_NOFF register per-queue; the multiplier is stored once in the NOTIFY capability.

4. **DSB SY placement**: Must appear after config writes and before reads that depend on those writes. Miss one and device state desynchronizes.

5. **Volatile semantics on ring indices**: The `used->idx` field must be read volatile (not cached by compiler) because the device advances it asynchronously.

6. **Available ring size calculation**: Descriptor table must be large enough for `VIRTQ_MAX_SIZE` elements. Available ring is `2 + 2*size` bytes (not allocated to the next power of 2).

7. **Polling timeout**: 10M iterations is empirically "safe enough" for QEMU but not architecturally guaranteed. Production code might need to tune or add timeout configuration.

8. **Feature negotiation rollback**: If device rejects features (FEATURES_OK bit not set after writing), cannot proceed. C code checks this; Rust port must do the same.

## References

- VirtIO Specification 1.2: https://docs.oasis-open.org/virtio/virtio/v1.2/os/virtio-v1.2-os.html
- ARM ARMv8 ISA Reference: Data Synchronization Barrier (DSB)
- PCI Express Base Specification: Capability List
