# VirtIO Balloon Driver Subsystem Specification

**Subsystem Key:** `balloon`

**Author Notes:** This is a pure-Rust port of the C virtio-balloon driver. All constants, register offsets, and memory layouts are reproduced exactly from the C source. See `git show a2f1104:src/pci/virtio/balloon/` for the original implementation.

---

## Overview

The virtio-balloon driver implements cooperative memory ballooning per the VirtIO Specification Section 5.5 (Device Type 5). It enables the host hypervisor to ask the guest kernel to temporarily hand over ("inflate") physical pages, which the host can then reuse elsewhere; the guest can later ask to reclaim ("deflate") those pages. This is a *host-initiated* capacity management mechanism; the driver does not (yet) wire a virtio-config-change interrupt, so inflate/deflate in this kernel are driver-initiated via user-space shell commands.

**Key invariants:**
- Balloon pages are always 4 KiB (2^12 bytes), per VirtIO spec, regardless of guest page size.
- PFN (Page Frame Number) is always `phys_addr >> 12`.
- The driver maintains a hard cap of 1024 pages (VIRTIO_BALLOON_MAX_PAGES), which trades memory footprint for reachability.
- All allocated pages are tracked internally; the user must *not* free them directly—only via deflate.
- Memory barrier (dsb sy) is issued after all MMIO writes to device registers to ensure ordering.

---

## Hardware Constants & Registers

### VirtIO PCI Device Identification

```c
#define VIRTIO_BALLOON_VENDOR_ID 0x1AF4       /* RedHat/VirtIO vendor */
#define VIRTIO_BALLOON_DEVICE_ID 0x1045       /* Device Type 5: 0x1040 + 5 */
```

### Balloon Page Size

```c
#define VIRTIO_BALLOON_PFN_SHIFT 12            /* Log2 of balloon page size */
/* Balloon pages are always 4096 bytes (2^12), per VirtIO spec */
/* PFN = phys_addr >> VIRTIO_BALLOON_PFN_SHIFT */
```

### VirtQueue Indices (per VirtIO Spec §5.5.2)

```c
#define VIRTIO_BALLOON_VQ_INFLATE 0            /* Queue for inflating balloon */
#define VIRTIO_BALLOON_VQ_DEFLATE 1            /* Queue for deflating balloon */
```

### Device Configuration Space Offsets (§5.5.4, all little-endian u32)

```c
#define VIRTIO_BALLOON_CFG_NUM_PAGES 0x00      /* Host's desired balloon size (read-only) */
#define VIRTIO_BALLOON_CFG_ACTUAL    0x04      /* Driver's actual balloon size (write-only) */
```

### Capacity Limits

```c
#define VIRTIO_BALLOON_MAX_PAGES 1024          /* Hard cap; drives tracking array size */
/* 1024 pages * 4 KiB = 4 MiB max balloon */
```

---

## Memory Layout & Structures

### Device State Structure

```c
struct virtio_balloon {
  struct pci_device pci;                      /* PCI device info + BARs */
  struct virtio_pci_caps pci_caps;            /* Parsed MMIO capability addresses */
  struct virtqueue inflate_vq;                /* Inflate queue descriptor */
  struct virtqueue deflate_vq;                /* Deflate queue descriptor */
  uint32_t actual;                            /* Mirrors CFG_ACTUAL; current balloon size in pages */
};
```

### Virtqueue Ring Structures (page-aligned, 4096-byte alignment)

These are defined in `virtqueue.h` but reproduced here for reference:

```c
struct virtq_desc {
  uint64_t addr;                              /* Physical address of buffer */
  uint32_t len;                               /* Buffer length in bytes */
  uint16_t flags;                             /* Descriptor flags (see below) */
  uint16_t next;                              /* Index of next descriptor (if VIRTQ_DESC_F_NEXT set) */
};

struct virtq_avail {
  uint16_t flags;                             /* Avail ring flags (0 = notify device) */
  uint16_t idx;                               /* Next available index */
  uint16_t ring[16];                          /* VIRTQ_MAX_SIZE = 16 descriptor indices */
};

struct virtq_used_elem {
  uint32_t id;                                /* Head descriptor index device used */
  uint32_t len;                               /* Bytes written by device */
};

struct virtq_used {
  uint16_t flags;                             /* Used ring flags (0 = interrupt on next update) */
  uint16_t idx;                               /* Next used index */
  struct virtq_used_elem ring[16];            /* VIRTQ_MAX_SIZE = 16 used elements */
};

#define VIRTQ_MAX_SIZE 16
#define VIRTQ_DESC_F_NONE  0                   /* No flags; device reads this buffer */
#define VIRTQ_DESC_F_NEXT  1                   /* Buffer continues via 'next' field */
#define VIRTQ_DESC_F_WRITE 2                   /* Device writes (vs reads) */
```

### Descriptor Rings (page-aligned)

In balloon.c, static allocations for both inflate and deflate queues:

```c
static struct virtq_desc inflate_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));           /* 16 * 16 bytes = 256 bytes, padded to 4096 */
static struct virtq_avail inflate_avail __attribute__((aligned(4096)));
static struct virtq_used inflate_used __attribute__((aligned(4096)));

static struct virtq_desc deflate_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail deflate_avail __attribute__((aligned(4096)));
static struct virtq_used deflate_used __attribute__((aligned(4096)));
```

### PFN Buffer (DMA-visible, 64-byte aligned)

```c
static uint32_t pfn_buf[VIRTIO_BALLOON_MAX_PAGES]
    __attribute__((aligned(64)));             /* Max 1024 u32 entries = 4096 bytes */
/* Holds PFNs to submit to device on inflate/deflate */
```

### Inflated Pages Tracking

```c
static uint32_t inflated_pfns[VIRTIO_BALLOON_MAX_PAGES];
/* Array of PFNs currently held by host; active prefix is [0..actual-1] */
/* Stored in allocation order (LIFO deallocation) */
```

---

## VirtIO Common Config Register Offsets

Used during device initialization (via the common config capability). These are in `virtio.h` but critical for balloon:

```c
#define VIRTIO_COMMON_DFSELECT 0x00            /* Device feature select (u32 rw) */
#define VIRTIO_COMMON_DF 0x04                  /* Device feature (u32 r) */
#define VIRTIO_COMMON_GFSELECT 0x08            /* Guest feature select (u32 rw) */
#define VIRTIO_COMMON_GF 0x0C                  /* Guest feature (u32 rw) */
#define VIRTIO_COMMON_MSIX 0x10                /* MSI-X config vector (u16 rw) */
#define VIRTIO_COMMON_NUMQ 0x12                /* Number of queues (u16 r) */
#define VIRTIO_COMMON_STATUS 0x14              /* Device status (u8 rw) */
#define VIRTIO_COMMON_CFGGEN 0x15              /* Config generation (u8 r) */
#define VIRTIO_COMMON_Q_SELECT 0x16            /* Queue select (u16 rw) */
#define VIRTIO_COMMON_Q_SIZE 0x18              /* Queue size (u16 rw) */
#define VIRTIO_COMMON_Q_MSIX 0x1A              /* Queue MSI-X vector (u16 rw) */
#define VIRTIO_COMMON_Q_ENABLE 0x1C            /* Queue enable (u16 rw) */
#define VIRTIO_COMMON_Q_NOFF 0x1E              /* Queue notify offset (u16 r) */
#define VIRTIO_COMMON_Q_DESCLO 0x20            /* Descriptor table address low (u32 rw) */
#define VIRTIO_COMMON_Q_DESCHI 0x24            /* Descriptor table address high (u32 rw) */
#define VIRTIO_COMMON_Q_DRIVERLO 0x28          /* Available ring address low (u32 rw) */
#define VIRTIO_COMMON_Q_DRIVERHI 0x2C          /* Available ring address high (u32 rw) */
#define VIRTIO_COMMON_Q_DEVICELO 0x30          /* Used ring address low (u32 rw) */
#define VIRTIO_COMMON_Q_DEVICEHI 0x34          /* Used ring address high (u32 rw) */

/* Device Status Bits */
#define VIRTIO_STATUS_RESET 0
#define VIRTIO_STATUS_ACKNOWLEDGE 1            /* Set after finding device */
#define VIRTIO_STATUS_DRIVER 2                 /* Set after reading features */
#define VIRTIO_STATUS_DRIVER_OK 4              /* Set after queues configured */
#define VIRTIO_STATUS_FEATURES_OK 8            /* Set after negotiating features; device validates */
#define VIRTIO_STATUS_FAILED 128               /* Device encountered error */
```

---

## Feature Negotiation

The balloon driver negotiates VirtIO features as follows:

```
Device features we ignore:
  - VIRTIO_F_NOTIFY_ON_EMPTY (bit 24): not used
  - VIRTIO_F_ANY_LAYOUT (bit 27): not used
  - VIRTIO_F_RING_INDIRECT_DESC (bit 28): not used
  - VIRTIO_F_RING_EVENT_IDX (bit 29): not used

Balloon-specific features we *do not* set:
  - F_MUST_TELL_HOST (bit 0): we always notify before reclaiming
  - F_STATS_VQ (bit 1): we don't expose memory stats yet
  - F_DEFLATE_ON_OOM (bit 2): we'd need PMM hooks
  - F_FREE_PAGE_HINT / F_PAGE_POISON / F_REPORTING (bits 3..5):
    all advanced extensions we'd implement later

Feature we DO set:
  - VIRTIO_F_VERSION_1 (bit 32 = GFSELECT=1, bit 0):
    Required for modern VirtIO devices
```

After reading device features, the driver:
1. Sets `GFSELECT` to 0, clears all bits
2. Sets `GFSELECT` to 1, sets only bit 0 (VIRTIO_F_VERSION_1)
3. Writes `FEATURES_OK` status
4. Reads back status to verify device accepted

---

## Public API

### `void pci_virtio_balloon_init(void)`

**Purpose:** Initialize the balloon device, perform VirtIO handshake, setup queues, and transition to DRIVER_OK state.

**Behavior:**
1. Search PCI bus for device (VENDOR_ID 0x1AF4, DEVICE_ID 0x1045)
2. If not found, log "[BALLOON] Device not found (skipping)" and return
3. Verify endpoint device type (header type 0x00)
4. Assign PCI BARs, enable device, parse VirtIO capability pointers
5. Reset device (set STATUS to 0)
6. Acknowledge device (set STATUS bit 1)
7. Set DRIVER status (set STATUS bit 2)
8. Negotiate features: accept ONLY VIRTIO_F_VERSION_1
9. Set FEATURES_OK status, verify device accepts
10. Configure inflate queue (index 0) via `virtqueue_setup()`
11. Configure deflate queue (index 1) via `virtqueue_setup()`
12. Set DRIVER_OK status
13. Initialize `actual` to 0, publish to device_cfg.actual
14. Set global `bln_ready` flag to 1
15. Log "[BALLOON] DRIVER_OK; host target=X pages, actual=0"

**Side effects:**
- Initializes static `bln_dev` structure
- Sets `bln_ready = 1` on success
- Issues MMIO writes with `dsb_sy()` barriers
- Allocates BARs and maps device config regions
- Initializes two virtqueues with pre-allocated descriptor rings

**Failure modes:**
- Device not found: silent skip (already common case)
- Header type mismatch: log error, return
- FEATURES_OK rejected: log "[BALLOON] FEATURES_OK rejected", return
- Virtqueue setup failure: log error for the failing queue, return

**Called from:** `kernel.c:main()` after `pci_enumerate_bus()`

---

### `int balloon_inflate(uint32_t n)`

**Purpose:** Hand up to `n` physical pages from the guest to the host.

**Parameters:**
- `n`: Requested number of pages to inflate (in 4 KiB balloon pages)

**Return value:**
- On success: number of pages actually inflated (0 ≤ result ≤ n)
- On error: -1 (device handshake failed or not ready)

**Behavior:**
1. If `!bln_ready`, return -1
2. Cap `n` to `VIRTIO_BALLOON_MAX_PAGES - bln_dev.actual` (headroom)
3. If capped `n` is now 0, return 0
4. For each of the (up to) `n` pages:
   - Call `pmm_allocate_page()` to get a physical address
   - If PMM is exhausted (`pa == 0`), stop here with what we got
   - Convert PA to PFN: `pfn = pa >> VIRTIO_BALLOON_PFN_SHIFT`
   - Store PFN in `pfn_buf[got]` (device sees these)
   - Store PFN in `inflated_pfns[bln_dev.actual + got]` (our tracking)
   - Increment `got`
5. If `got == 0`, return 0 (no pages available)
6. Submit the `got` PFNs via `submit_pfn_batch(&bln_dev.inflate_vq, got)`:
   - If device handshake fails (submit_pfn_batch returns -1):
     - Free all allocated pages back to PMM
     - Return -1
7. On success:
   - Increment `bln_dev.actual` by `got`
   - Call `publish_actual()` to write new actual size to device_cfg.actual
   - Return `(int)got`

**Synchronization:** Not thread-safe; assumes single-threaded or externally synchronized access.

**Called from:** Shell command handler for `balloon inflate N`

---

### `int balloon_deflate(uint32_t n)`

**Purpose:** Reclaim up to `n` pages from the host back into the PMM free pool.

**Parameters:**
- `n`: Requested number of pages to deflate

**Return value:**
- On success: number of pages actually deflated (0 ≤ result ≤ bln_dev.actual)
- On error: -1 (device handshake failed or not ready)

**Behavior:**
1. If `!bln_ready`, return -1
2. Cap `n` to `bln_dev.actual` (can't deflate more than we have)
3. If capped `n` is now 0, return 0
4. Calculate base index: `base = bln_dev.actual - n`
5. Copy the most recently inflated `n` PFNs from `inflated_pfns[base..base+n-1]` into `pfn_buf[0..n-1]` (LIFO order)
6. Submit the `n` PFNs via `submit_pfn_batch(&bln_dev.deflate_vq, n)`:
   - If device handshake fails, return -1 and leave balloon state unchanged
7. On success:
   - For each of the `n` pages, call `pmm_free_page()` with the PA (PFN << 12)
   - Decrement `bln_dev.actual` by `n`
   - Call `publish_actual()` to write new actual size to device_cfg.actual
   - Return `(int)n`

**Synchronization:** Not thread-safe.

**Called from:** Shell command handler for `balloon deflate N`

---

### `void balloon_get_status(uint32_t *actual_pages, uint32_t *host_target)`

**Purpose:** Query current balloon state for status display (e.g., `/proc/balloon`, shell status).

**Parameters:**
- `actual_pages`: Pointer to u32 (may be NULL). On success, filled with current inflated page count.
- `host_target`: Pointer to u32 (may be NULL). On success, filled with host's desired page count from device_cfg.num_pages.

**Behavior:**
1. If `actual_pages` is not NULL:
   - Write `bln_dev.actual` if `bln_ready`, else write 0
2. If `host_target` is not NULL:
   - If `!bln_ready` or `bln_dev.pci_caps.device_cfg == 0`:
     - Write 0
   - Else:
     - Read `device_cfg.num_pages` (CFG_NUM_PAGES = 0x00) and write to `*host_target`

**Synchronization:** Read-only; safe to call concurrently if balloon is not being inflated/deflated.

**Called from:** Shell command handler for `balloon status` and diagnostic tools

---

## Implementation Details: `submit_pfn_batch()`

**Purpose:** Internal helper that submits a batch of PFNs on a virtqueue and waits for device acknowledgment.

```c
static int submit_pfn_batch(struct virtqueue *vq, uint32_t count)
```

**Parameters:**
- `vq`: Pointer to target virtqueue (inflate or deflate)
- `count`: Number of PFNs in `pfn_buf` to submit

**Return value:** 0 on success, -1 on failure (device timeout)

**Behavior:**
1. If `count == 0`, return 0
2. Calculate physical address of `pfn_buf`: `pa = VIRT_TO_PHYS((uintptr_t)pfn_buf)`
3. Submit single descriptor via `virtqueue_submit(vq, pa, count * sizeof(u32), VIRTQ_DESC_F_NONE)`:
   - Buffer is device-readable (no VIRTQ_DESC_F_WRITE flag)
   - Size is `count * 4` bytes (one u32 per PFN)
4. Ring doorbell via `virtqueue_notify(vq)`
5. Poll for completion via `virtqueue_poll(vq)`:
   - Waits for device to write a used entry
   - Returns 0 bytes on read-only buffer (no data written back)
   - Has internal spin counter and logs timeout
6. Return 0 (success or timeout already logged)

**Synchronization:** Assumes single submission per queue at a time.

---

## Implementation Details: `publish_actual()`

**Purpose:** Mirror internal `bln_dev.actual` size into the device config space.

```c
static void publish_actual(void)
```

**Behavior:**
1. Extract device config base address from `bln_dev.pci_caps.device_cfg`
2. If base is 0 (cap not advertised), return silently
3. Write `bln_dev.actual` to offset CFG_ACTUAL (0x04) via `mmio_write32()`
4. Issue `dsb_sy()` barrier to ensure write ordering

**Rationale:** The VirtIO spec (§5.5.6) notes that this field is informational; the host uses it for statistics, not control. However, we update it after every inflate/deflate to keep the host's view consistent.

---

## Syscall Integration

The balloon subsystem is exposed via the `SYS_BALLOON` syscall (ABI number 14):

```c
static inline int64_t sys_balloon(uint64_t op, uint64_t n)
```

**Operation codes (op parameter):**
- `0`: INFLATE — call `balloon_inflate((uint32_t)n)`, return result
- `1`: DEFLATE — call `balloon_deflate((uint32_t)n)`, return result
- `2`: ACTUAL — return current `bln_dev.actual` (or 0 if not ready)
- `3`: TARGET — call `balloon_get_status()` and return host target

**Result:** Syscall returns the signed result directly to user space.

---

## Rust Porting Strategy

### Module Structure
```
kernel::drivers::balloon
├── mod.rs                    # Module root, public API, initialization
├── device.rs                 # BalloonDevice struct, device state
├── virtqueue.rs              # Virtqueue management (import from virtio crate)
└── constants.rs              # All #defines from balloon.h, virtio.h, virtqueue.h
```

### Core Types

#### `BalloonDevice`
```rust
pub struct BalloonDevice {
    pci: PciDevice,
    pci_caps: VirtioPciCaps,
    inflate_vq: Virtqueue,
    deflate_vq: Virtqueue,
    actual: u32,
}
```

#### State Management
```rust
static BALLOON: Mutex<Option<BalloonDevice>> = Mutex::new(None);
static READY: AtomicBool = AtomicBool::new(false);
```

### Key Design Decisions

1. **Synchronization:** Use `Mutex<Option<BalloonDevice>>` to gate access. `balloon_inflate()` and `balloon_deflate()` must acquire the lock.

2. **Static Buffers:** Replicate C's page-aligned statics exactly:
   ```rust
   #[repr(align(4096))]
   struct InflateQueues {
       desc: [VirtqDesc; 16],
       avail: VirtqAvail,
       used: VirtqUsed,
   }
   static INFLATE_QUEUES: InflateQueues = ...;
   ```

3. **PFN Buffer:** Keep as `[u32; VIRTIO_BALLOON_MAX_PAGES]` with 64-byte alignment.

4. **Inflated Tracking:** Store as `Vec<u32>` with capacity pre-allocated to MAX_PAGES, or keep as array with a length counter.

5. **MMIO Operations:** Use existing `mmio::read32()` / `mmio::write32()` wrappers (or port them if not yet available).

6. **Barriers:** Use inline asm: `core::arch::asm!("dsb sy", options(nostack, preserves_flags))`.

7. **PMM Integration:** Call existing PMM functions:
   ```rust
   extern "C" {
       pub fn pmm_allocate_page() -> uintptr_t;
       pub fn pmm_free_page(phys_addr: uintptr_t);
   }
   ```

8. **Virtqueue Submission:** Reuse existing `Virtqueue` struct and submission helpers from the virtio crate.

### Assembly Requirements

- `dsb_sy()`: Single inline asm instruction, no heap/allocator needed
- Virtual-to-physical translation (`VIRT_TO_PHYS`): Macro, no asm
- MMIO operations: Wrapped volatile pointer dereferences, no asm needed
- All else: Pure Rust possible

No `.S` files needed; inline asm is sufficient for memory barriers.

---

## Boot/Usage Ordering

1. **Early boot (kernel_init):** Kernel MMIO window is set up (lower-half identity map active)
2. **PCI discovery (kernel main):** `pci_enumerate_bus()` runs; finds devices
3. **Balloon init:** `pci_virtio_balloon_init()` called; device enters DRIVER_OK
4. **Userspace shell:** Once scheduler and syscall dispatch active, users can type `balloon status`, `balloon inflate N`, `balloon deflate N`

**Prerequisite subsystems:**
- PCI enumeration + BAR assignment
- VirtIO capability parsing
- Virtqueue setup infrastructure
- PMM allocator
- MMIO (read/write) operations
- Syscall dispatch for SYS_BALLOON

**Dependent subsystems:**
- Shell command handler (uses syscall)
- Proc filesystem (status reporting)

---

## Gotchas & Correctness Issues

1. **Memory Barrier Ordering:** Every MMIO write to device registers MUST be followed by `dsb_sy()` to ensure the write is visible to the device before we proceed. Missing barriers can cause the device to see stale register values or out-of-order updates.

2. **Page Accounting:** If `submit_pfn_batch()` fails midway (e.g., timeout), the C code *frees pages back to PMM* before returning -1. The Rust port MUST replicate this—otherwise pages leak.

3. **LIFO Deflation:** Deflation uses LIFO (pop most-recent pages first), not FIFO. This matters for memory fragmentation and page reuse patterns. The code copies pages in order from `inflated_pfns[base..base+n]`, which maintains LIFO order.

4. **PFN Encoding:** PFN is always `phys_addr >> 12`. Ensure shift direction is correct; a PA of 0x1000 should encode as PFN 1, not 0x1000.

5. **Device Config Offset:** `VIRTIO_BALLOON_CFG_NUM_PAGES` is 0x00, `CFG_ACTUAL` is 0x04. These are both relative to `device_cfg` base, not the BAR start. Misalignment here silently reads garbage.

6. **Feature Negotiation Endianness:** Device/guest feature regs are little-endian u32. Feature bit 32 (VIRTIO_F_VERSION_1) requires writing to GFSELECT=1 first. Trying to set it in GFSELECT=0 (bits 0-31) will fail silently.

7. **Virtqueue Size:** VIRTQ_MAX_SIZE is 16. If we ever increase it, we must re-align all the descriptor rings and ensure pfn_buf is large enough.

8. **Single-threaded Assumption:** The current C code assumes single-threaded access. The Rust port must add explicit locking (Mutex) if concurrency is added later. Status queries (`balloon_get_status()`) can be lock-free reads if implemented carefully.

9. **Ring Alignment:** All descriptor, avail, and used rings MUST be page-aligned (4096-byte alignment). Many of these fields are adjacent in memory; padding or reordering will break the layout.

10. **Timeout Handling:** `virtqueue_poll()` already logs timeouts internally. Do not add extra error messages that duplicate the log output.

11. **Device Not Found is Not an Error:** If the device is not found during init, that is normal (not all QEMU configs provide virtio-balloon). Do not treat it as a failure condition.

12. **Host Target is Read-Only:** We can read `device_cfg.num_pages` but must not write to it. That field is host-controlled.

---

## References & Specs

- **VirtIO Specification:** Section 5.5 (Memory Balloon Device)
- **Original C Source:** `/src/pci/virtio/balloon/balloon.{c,h}` (commit a2f1104)
- **Related Subsystems:**
  - PCI enumeration & BAR assignment
  - VirtIO capability parsing & common config
  - Virtqueue setup & submission
  - PMM (physical page allocator)
  - MMIO read/write wrappers
  - Syscall dispatch (SYS_BALLOON = 14)

