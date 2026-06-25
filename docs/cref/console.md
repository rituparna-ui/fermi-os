# virtio-console Driver Specification

## Overview

The virtio-console subsystem provides a simple, TX-only logging interface to a QEMU character device backend. The driver implements the VirtIO console device specification (VirtIO 1.x), configured in non-MULTIPORT mode with two virtqueues:
- Queue 0 (RX): host-to-guest data (configured but not actively used)
- Queue 1 (TX): guest-to-host data (actively used for logging)

The TX path accepts byte buffers, fragments them into 4 KiB chunks to fit the DMA staging buffer, and polls the device for completion synchronously. All data written to the TX queue is forwarded by QEMU to a host-side file (`build/virtio-console.txt`), providing a separate logging channel from the PL011 UART.

**Subsystem Key:** `console`

## Constants & Hardware Details

### VirtIO Device IDs
```c
#define VIRTIO_CONSOLE_VENDOR_ID 0x1AF4      /* Red Hat vendor ID */
#define VIRTIO_CONSOLE_DEVICE_ID 0x1043      /* Modern PCI device ID for console */
```

### Virtqueue Indices (Non-MULTIPORT Mode)
```c
#define VIRTIO_CONSOLE_VQ_RX 0                /* receiveq(0): host → guest */
#define VIRTIO_CONSOLE_VQ_TX 1                /* transmitq(0): guest → host */
```

### Device Configuration Space (§5.3.4 of VirtIO spec)
```c
#define VIRTIO_CONSOLE_CFG_COLS         0x00  /* u16 le: terminal columns (not read) */
#define VIRTIO_CONSOLE_CFG_ROWS         0x02  /* u16 le: terminal rows (not read) */
#define VIRTIO_CONSOLE_CFG_MAX_NR_PORTS 0x04  /* u32 le: max ports (MULTIPORT only) */
#define VIRTIO_CONSOLE_CFG_EMERG_WR     0x08  /* u32 le: emergency write (not used) */
```

### DMA Staging Buffer
```c
#define CONSOLE_TX_BUF 4096                   /* Maximum bytes per single TX fragment */
```

### VirtIO Common Config Register Offsets (§4.1.4.3)
```c
#define VIRTIO_COMMON_DFSELECT    0x00        /* u32 rw: device feature select */
#define VIRTIO_COMMON_DF          0x04        /* u32 r:  device feature bits */
#define VIRTIO_COMMON_GFSELECT    0x08        /* u32 rw: driver (guest) feature select */
#define VIRTIO_COMMON_GF          0x0C        /* u32 rw: driver feature bits */
#define VIRTIO_COMMON_MSIX        0x10        /* u16 rw: MSI-X config vector */
#define VIRTIO_COMMON_NUMQ        0x12        /* u16 r:  number of virtqueues */
#define VIRTIO_COMMON_STATUS      0x14        /* u8  rw: device status */
#define VIRTIO_COMMON_CFGGEN      0x15        /* u8  r:  config generation */
#define VIRTIO_COMMON_Q_SELECT    0x16        /* u16 rw: queue select */
#define VIRTIO_COMMON_Q_SIZE      0x18        /* u16 rw: queue size */
#define VIRTIO_COMMON_Q_MSIX      0x1A        /* u16 rw: queue MSI-X vector */
#define VIRTIO_COMMON_Q_ENABLE    0x1C        /* u16 rw: queue enable */
#define VIRTIO_COMMON_Q_NOFF      0x1E        /* u16 r:  queue notify offset */
#define VIRTIO_COMMON_Q_DESCLO    0x20        /* u32 rw: descriptor table addr (low) */
#define VIRTIO_COMMON_Q_DESCHI    0x24        /* u32 rw: descriptor table addr (high) */
#define VIRTIO_COMMON_Q_DRIVERLO  0x28        /* u32 rw: available ring addr (low) */
#define VIRTIO_COMMON_Q_DRIVERHI  0x2C        /* u32 rw: available ring addr (high) */
#define VIRTIO_COMMON_Q_DEVICELO  0x30        /* u32 rw: used ring addr (low) */
#define VIRTIO_COMMON_Q_DEVICEHI  0x34        /* u32 rw: used ring addr (high) */
```

### Device Status Bits
```c
#define VIRTIO_STATUS_RESET       0            /* 0x00 */
#define VIRTIO_STATUS_ACKNOWLEDGE 1            /* 0x01 */
#define VIRTIO_STATUS_DRIVER      2            /* 0x02 */
#define VIRTIO_STATUS_DRIVER_OK   4            /* 0x04 */
#define VIRTIO_STATUS_FEATURES_OK 8            /* 0x08 */
#define VIRTIO_STATUS_FAILED      128          /* 0x80 */
```

### Feature Bits (Feature Select 0 = lower 32-bit features)
```c
/* Driver accepts ONLY VIRTIO_F_VERSION_1 (bit 32, accessed via feature select 1 / bit 0) */
#define VIRTIO_F_VERSION_1 (1u << 0)  /* When feature select = 1, accept this bit */

/* Device-specific features (rejected by driver): */
#define VIRTIO_CONSOLE_F_SIZE       (1u << 0)  /* host-set cols/rows */
#define VIRTIO_CONSOLE_F_MULTIPORT  (1u << 1)  /* multi-port control queue */
#define VIRTIO_CONSOLE_F_EMERG_WRITE (1u << 2) /* emergency write register */
```

### Virtqueue Descriptor Flags
```c
#define VIRTQ_DESC_F_NONE  0                   /* device reads buffer */
#define VIRTQ_DESC_F_NEXT  1                   /* descriptor chains continue */
#define VIRTQ_DESC_F_WRITE 2                   /* device writes (vs reads) */
```

### MSI-X and Polling Mode
```c
#define VIRTIO_MSI_NO_VECTOR 0xFFFF            /* Disable MSI-X, use polling */
#define VIRTQ_MAX_SIZE       16                /* Maximum queue size */
```

### Memory Management
```c
#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL /* Upper-half kernel mapping offset */
#define PHYS_TO_VIRT(pa) ((pa) + KERNEL_VA_OFFSET)
#define VIRT_TO_PHYS(va) ((va) - KERNEL_VA_OFFSET)
```

### Virtqueue Memory Layout

**Descriptor Table:** 16 descriptors × 16 bytes = 256 bytes, 4 KiB aligned
- Descriptor structure:
  ```c
  struct virtq_desc {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length in bytes */
    uint16_t flags;     /* VIRTQ_DESC_F_* */
    uint16_t next;      /* Next descriptor index (if NEXT flag set) */
  };
  ```

**Available Ring:** 2-byte flags + 2-byte index + 16 × 2-byte entries = 36 bytes, 4 KiB aligned
- Available ring structure:
  ```c
  struct virtq_avail {
    uint16_t flags;     /* VIRTQ_AVAIL_F_NO_INTERRUPT (not used) */
    uint16_t idx;       /* Next available ring index to populate */
    uint16_t ring[16];  /* Descriptor indices available to device */
  };
  ```

**Used Ring:** 2-byte flags + 2-byte index + 16 × (4-byte id + 4-byte len) = 100 bytes, 4 KiB aligned
- Used ring entry:
  ```c
  struct virtq_used_elem {
    uint32_t id;        /* Descriptor index consumed by device */
    uint32_t len;       /* Bytes written by device (for RX) or written by device (for TX, usually 0) */
  };
  struct virtq_used {
    uint16_t flags;
    uint16_t idx;       /* Next used ring index (incremented by device) */
    struct virtq_used_elem ring[16];
  };
  ```

### PCI Configuration Space
```c
#define PCI_VENDOR_ID    0x00  /* u16 */
#define PCI_DEVICE_ID    0x02  /* u16 */
#define PCI_COMMAND      0x04  /* u16 */
#define PCI_STATUS       0x06  /* u16 */
#define PCI_HEADER_TYPE  0x0E  /* u8 */
#define PCI_BAR0         0x10  /* u32 (or u64 for 64-bit) */
#define PCI_CAP_PTR      0x34  /* u8: capability pointer */

#define PCI_ENDPOINT_DEV_TYPE 0x00
```

### PCI BAR and Capability Layout

Modern VirtIO PCI devices expose four capability structures (in common config BAR):
1. **Common Config** (type 1): Device setup, feature negotiation, queue config
2. **Notify Config** (type 2): Per-queue doorbell registers
3. **ISR Config** (type 3): Interrupt status (not used; polling mode)
4. **Device Config** (type 4): Device-specific configuration (console: cols/rows)

The driver must parse these from the PCI capability chain (offset 0x34 = first capability pointer).

## Data Structures

### struct virtio_console
```c
struct virtio_console {
  struct pci_device pci;           /* PCI device info (bus/slot/func/vendor/device) */
  struct virtio_pci_caps pci_caps; /* Parsed PCI capability addresses */
  struct virtqueue tx_vq;          /* TX virtqueue (guest → host) */
  struct virtqueue rx_vq;          /* RX virtqueue (host → guest, unconfigured) */
};
```

### struct virtio_pci_caps
```c
struct virtio_pci_caps {
  uintptr_t common_cfg;              /* VA of VirtIO common config BAR */
  uintptr_t notify_base;             /* VA of notification register BAR */
  uintptr_t isr_cfg;                 /* VA of ISR status BAR (not used) */
  uintptr_t device_cfg;              /* VA of device config BAR (not read) */
  uint32_t notify_off_multiplier;    /* Notify address = notify_base + queue_id * notify_off * multiplier */
};
```

### struct virtqueue
```c
struct virtqueue {
  uint16_t size;           /* Negotiated queue size (≤ VIRTQ_MAX_SIZE) */
  uint16_t free_head;      /* Next descriptor index to allocate */
  uint16_t last_used;      /* Last used.idx we've observed (for polling) */
  uintptr_t notify_addr;   /* Physical address of doorbell register for this queue */
  
  struct virtq_desc *desc;    /* Descriptor table (VA, 4 KiB aligned) */
  struct virtq_avail *avail;  /* Available ring (VA, 4 KiB aligned) */
  struct virtq_used *used;    /* Used ring (VA, 4 KiB aligned) */
};
```

## Boot/Initialization Sequence

### 1. Device Discovery & Enumeration (pci_virtio_console_init)

Called once at kernel startup from `kernel_main()` after UART is online and MMU is enabled.

**Step 1.1: Find Device**
```c
pci_find_device(VIRTIO_CONSOLE_VENDOR_ID, VIRTIO_CONSOLE_DEVICE_ID, &con_dev.pci)
```
- Scans pre-enumerated PCI device list for console vendor ID (0x1AF4) and device ID (0x1043)
- If not found, log "[CONSOLE] Device not found (skipping)" and return
- Failure is not fatal; driver is simply unavailable

**Step 1.2: Verify Header Type**
```c
if ((pci_get_header_type(&con_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE)
```
- Endpoint device type = 0x00
- If wrong, log "[CONSOLE] Unexpected header type" and return

**Step 1.3: Assign BARs and Enable Device**
```c
pci_assign_bars(&con_dev.pci);
pci_enable_device(&con_dev.pci);
```
- Map PCI BAR addresses to physical addresses
- Set PCI_COMMAND.MASTER and MEMORY_SPACE bits to enable DMA and MMIO

**Step 1.4: Parse VirtIO Capabilities**
```c
virtio_parse_capabilities(&con_dev.pci, &con_dev.pci_caps);
```
- Parse PCI capability chain to find common_cfg, notify_base, isr_cfg, device_cfg
- Calculate notify_off_multiplier from notify capability

### 2. Device Initialization (Status Machine per VirtIO §3.1)

All offsets are relative to `common_cfg` base address.

**Step 2.1: Reset Device**
```c
mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);  /* 0 */
dsb_sy();
while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) { }
```
- Write RESET (0) to status
- Issue full data synchronization barrier (dsb sy)
- Poll until device confirms reset

**Step 2.2: Acknowledge + Driver**
```c
uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);  /* |= 1 */
dsb_sy();
status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);  /* |= 2 */
dsb_sy();
```
- Set ACKNOWLEDGE (1) and DRIVER (2) bits using read-modify-write
- Barrier after each write

**Step 2.3: Feature Negotiation**

Read device features (two 32-bit halves):
```c
mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);  /* Select bits [31:0] */
dsb_sy();
uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);

mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);  /* Select bits [63:32] */
dsb_sy();
uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
```

Write driver-accepted features (VIRTIO_F_VERSION_1 only):
```c
uint32_t guest_lo = 0;                            /* Reject all lower features */
uint32_t guest_hi = feat_hi & 0x01;               /* Accept only VIRTIO_F_VERSION_1 */

mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
dsb_sy();
mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
dsb_sy();

mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
dsb_sy();
mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
dsb_sy();
```

**Step 2.4: Features OK**
```c
uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);  /* |= 8 */
dsb_sy();

status = mmio_read8(base + VIRTIO_COMMON_STATUS);
if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
  uart_errorln("[CONSOLE] FEATURES_OK rejected");
  return;
}
```
- Device must re-confirm FEATURES_OK; if not set after readback, device rejected negotiation

**Step 2.5: Configure RX Queue (Queue 0)**

via `virtqueue_setup(base, VIRTIO_CONSOLE_VQ_RX, &con_dev.rx_vq, &con_dev.pci_caps)`:

1. Disable MSI-X:
   ```c
   mmio_write16(base + VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);  /* 0xFFFF */
   dsb_sy();
   ```

2. Select and query queue:
   ```c
   mmio_write16(base + VIRTIO_COMMON_Q_SELECT, VIRTIO_CONSOLE_VQ_RX);  /* 0 */
   dsb_sy();
   uint16_t max_size = mmio_read16(base + VIRTIO_COMMON_Q_SIZE);
   ```

3. Set size and disable queue MSI-X:
   ```c
   uint16_t qsize = min(VIRTQ_MAX_SIZE, max_size);  /* Usually 16 */
   mmio_write16(base + VIRTIO_COMMON_Q_SIZE, qsize);
   mmio_write16(base + VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
   dsb_sy();
   ```

4. Initialize ring memory (zero-fill):
   ```c
   memset(rx_desc, 0, sizeof(rx_desc));
   memset(&rx_avail, 0, sizeof(rx_avail));
   memset(&rx_used, 0, sizeof(rx_used));
   ```

5. Write physical addresses (split into 32-bit halves):
   ```c
   uint64_t desc_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)rx_desc);
   mmio_write32(base + VIRTIO_COMMON_Q_DESCLO, (uint32_t)(desc_pa & 0xFFFFFFFF));
   mmio_write32(base + VIRTIO_COMMON_Q_DESCHI, (uint32_t)(desc_pa >> 32));
   
   uint64_t avail_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)&rx_avail);
   mmio_write32(base + VIRTIO_COMMON_Q_DRIVERLO, (uint32_t)(avail_pa & 0xFFFFFFFF));
   mmio_write32(base + VIRTIO_COMMON_Q_DRIVERHI, (uint32_t)(avail_pa >> 32));
   
   uint64_t used_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)&rx_used);
   mmio_write32(base + VIRTIO_COMMON_Q_DEVICELO, (uint32_t)(used_pa & 0xFFFFFFFF));
   mmio_write32(base + VIRTIO_COMMON_Q_DEVICEHI, (uint32_t)(used_pa >> 32));
   dsb_sy();
   ```

6. Calculate notify address:
   ```c
   uint16_t notify_off = mmio_read16(base + VIRTIO_COMMON_Q_NOFF);
   rx_vq.notify_addr = pci_caps->notify_base + (notify_off * pci_caps->notify_off_multiplier);
   ```

7. Enable queue:
   ```c
   mmio_write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
   dsb_sy();
   ```

**Step 2.6: Configure TX Queue (Queue 1)**

Same as RX, but with `VIRTIO_CONSOLE_VQ_TX = 1` and `tx_desc`, `tx_avail`, `tx_used`.

**Step 2.7: DRIVER_OK**
```c
uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);  /* |= 4 */
dsb_sy();
```

**Step 2.8: Mark Ready & Send Boot Banner**
```c
con_ready = 1;
const char banner[] = "[Fermi OS] virtio-console attached. Hello from guest!\n";
vcons_send(banner, sizeof(banner) - 1);
```

## Public API

### int vcons_send(const void *buf, uint32_t len)

**Purpose:** Send `len` bytes from `buf` to the host via the TX virtqueue.

**Signature:**
```c
int vcons_send(const void *buf, uint32_t len);
```

**Parameters:**
- `buf`: Kernel virtual address of byte buffer to send
- `len`: Number of bytes to send (0 is accepted)

**Return Value:**
- On success: `(int)len` — number of bytes accepted
- If driver not ready (`con_ready == 0`): `-1`
- If `len == 0`: `0` (no-op)

**Behavior:**

1. Return -1 if `con_ready` is false
2. Return 0 if `len` is 0
3. Fragment writes into 4 KiB chunks:
   - While `done < len`:
     - `chunk = min(CONSOLE_TX_BUF, len - done)`
     - Copy `chunk` bytes from `src + done` to `tx_buf` (DMA staging buffer)
     - Build single TX descriptor: `tx_buf` PA, length `chunk`, flags 0 (device reads)
     - Submit to TX virtqueue via `virtqueue_submit()`
     - Ring doorbell via `virtqueue_notify()`
     - **Poll for completion** via `virtqueue_poll()` — blocks until device consumes
     - Increment `done` by `chunk`
4. Return `(int)done` (always equals `len` on success)

**Important Notes:**
- **Synchronous, blocking:** Each fragment polls the device until completion before returning
- **Single in-flight descriptor:** Polling prevents two concurrent TX operations
- **Fragmentation is transparent:** Host sees continuous byte stream, unaware of 4 KiB boundaries
- **TX descriptor has no header:** VirtIO console TX payload is raw bytes (§5.3.6.4)
- **Memory ordering:** `dsb_sy()` issued by virtqueue code (not caller) before reading `used.idx`

### void pci_virtio_console_init(void)

**Purpose:** Initialize the virtio-console device at boot.

**Signature:**
```c
void pci_virtio_console_init(void);
```

**Parameters:** None

**Return Value:** None (void)

**Behavior:**

1. Log "[CONSOLE] Initializing Device"
2. Attempt to find device; if not found, log and return (not fatal)
3. Verify endpoint device type; if wrong, log and return
4. Assign PCI BARs and enable device
5. Parse VirtIO PCI capabilities
6. Execute device initialization state machine (reset → DRIVER_OK)
7. Configure both RX and TX virtqueues (RX is posted but not used)
8. Set `con_ready = 1`
9. Send boot banner via `vcons_send()`

**Failure Modes (all logged and non-fatal):**
- Device not found
- Unexpected PCI header type
- Feature negotiation failure (FEATURES_OK rejected)
- RX or TX queue setup failure

## Subsystem Dependencies

### Depends On
- **PCI Bus** (`pci`): Device enumeration, BAR assignment, capability parsing
- **PCI VirtIO** (`virtio`): Capability structures, common config offsets
- **Virtqueue** (`virtqueue`): `virtqueue_setup()`, `virtqueue_submit()`, `virtqueue_notify()`, `virtqueue_poll()`
- **MMIO** (`mmio`): Memory-mapped I/O reads/writes (mmio_read/write8/16/32)
- **MMU** (`mmu`): VIRT_TO_PHYS conversion, upper-half kernel VA mapping
- **CPU Barriers** (`utils`): `dsb_sy()` data synchronization barrier
- **UART** (`uart`): Logging/boot messages
- **String Utilities** (`strings`): memcpy, memset

### Depended On By
- **VFS/Device Layer** (`devices`): Exposes `/dev/vcons` character device
- **Shell/User Space** (`kernel` shell loop): `vlog` command and demo logging
- **Any subsystem requiring auxilliary logging:** RNG, block, network, balloon drivers may use for diagnostics

## Boot Ordering Requirements

1. **UART must be initialized** before calling `pci_virtio_console_init()` — all debug output goes to UART
2. **PCI must be enumerated** before console init — must discover device on bus
3. **MMU must be active** with upper-half kernel mapping — MMIO layer assumes kernel VA offset
4. **Memory allocator must be functional** — RX/TX ring structures are allocated on stack (static)
5. **CPU identification** should complete before console init (informational only)

**Actual Call Order from kernel.c:**
```c
uart_init();
pci_enumerate_bus();
pci_virtio_rng_init();
pci_virtio_blk_init();
pci_virtio_net_init();
pci_virtio_balloon_init();
pci_virtio_console_init();    /* ← Here, after other PCI devices */
```

## Critical Implementation Notes (Gotchas)

### 1. Memory Barriers & Device Synchronization

**MUST** use `dsb_sy()` (data synchronization barrier, full system) before reading volatile device state:
- Before reading device status register after writing it
- Before reading `used.idx` after polling loop (device has written it)
- After writing available ring entries (`avail->idx`)
- After writing any descriptor table entries

**Not a performance hazard** — VirtIO operations complete in microseconds; barrier latency negligible.

### 2. Feature Negotiation: Mandatory Version Bit

The driver **MUST** accept `VIRTIO_F_VERSION_1` (bit 32, feature select 1 / bit 0). Rejecting this bit causes some devices to refuse to initialize. The console device has no mandatory console-specific features (all are optional: SIZE, MULTIPORT, EMERG_WRITE), so only VERSION_1 is accepted.

### 3. Physical Address Conversion

Console driver converts KVAs to PAs via `VIRT_TO_PHYS()` macro for DMA descriptors. This works only when:
- Kernel is mapped at upper-half VA (`0xFFFF000000000000 + PA`)
- `KERNEL_VA_OFFSET = 0xFFFF000000000000`
- MMU is enabled (early boot uses identity mapping, then switches)

Calling `vcons_send()` before MMU enables upper-half mapping will corrupt descriptors.

### 4. RX Queue Unconfigured

RX queue **MUST** be configured (descriptor table addresses written) even though no buffers are posted. VirtIO spec requires every advertised queue to be set up before DRIVER_OK. Skipping RX setup may cause device to refuse DRIVER_OK or enter failed state.

**However:** RX buffers are never posted (`avail->idx` stays 0), so any data the host sends will queue up at device but not be received. This is correct for TX-only logging.

### 5. Polling Timeout

`virtqueue_poll()` spins up to 10,000,000 iterations before giving up. For QEMU virtio-console, completion happens in <100 iterations (microseconds). The timeout prevents kernel hang on wedged device; hitting it indicates severe device/QEMU malfunction.

### 6. DMA Staging Buffer Alignment

TX staging buffer `tx_buf[4096]` is 64-byte aligned (not 4 KiB) because only the first descriptor of a chain must be aligned for `virtqueue_submit()`. The buffer itself is page-aligned as a static (linker places it), but the alignment attribute is conservative (sufficient for DMA).

### 7. TX Descriptor Flags

TX descriptors use `VIRTQ_DESC_F_NONE` (0), indicating device reads the buffer. **Never** set `VIRTQ_DESC_F_WRITE` for TX (that's for RX where device writes data). Setting it would cause device to ignore the descriptor or produce undefined behavior.

### 8. Fragmentation is Transparent

`vcons_send()` may split a large write into multiple virtqueue submissions. Each fragment is a separate transaction (notify/poll cycle). QEMU coalesces received data at the file output, so the host sees a continuous stream unaware of internal fragmentation.

## Rust Porting Strategy

### Module Structure
```rust
pub mod console {
    pub struct Console { ... }
    pub fn init() -> Result<(), Error>;
    pub fn send(buf: &[u8]) -> Result<usize, Error>;
    pub fn is_ready() -> bool;
}
```

### Ownership & Statics

**Singleton Device State** (mutable static, guarded by mutex or UnsafeCell for early boot):
- Single `Console` struct per system
- Wrapped in `Mutex<Option<Console>>` or `static mut` with unsafe blocks (justified for single-threaded early boot)
- Once initialized at boot, no concurrent access patterns (blocking `vcons_send` prevents re-entrance)

**Ring Memory** (static buffers, page-aligned):
```rust
#[repr(align(4096))]
struct RxRings {
    desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    avail: VirtqAvail,
    used: VirtqUsed,
}

#[repr(align(4096))]
struct TxRings {
    desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    avail: VirtqAvail,
    used: VirtqUsed,
}

#[repr(align(64))]
struct TxBuffer([u8; CONSOLE_TX_BUF]);

static RX_RINGS: RxRings = /* zeroed */;
static TX_RINGS: TxRings = /* zeroed */;
static TX_BUFFER: TxBuffer = /* zeroed */;
static CONSOLE: Mutex<Option<Console>> = Mutex::new(None);
```

### Struct Layout

```rust
pub struct Console {
    pci_device: pci::Device,
    pci_caps: virtio::Caps,
    tx_vq: Virtqueue,
    rx_vq: Virtqueue,
}

struct Virtqueue {
    size: u16,
    free_head: u16,
    last_used: u16,
    notify_addr: PhysAddr,
    desc: &'static mut [VirtqDesc; VIRTQ_MAX_SIZE],
    avail: &'static mut VirtqAvail,
    used: &'static mut VirtqUsed,
}

struct VirtqDesc {
    addr: u64,
    len: u32,
    flags: u16,
    next: u16,
}

struct VirtqAvail {
    flags: u16,
    idx: AtomicU16,  // Device may read concurrently
    ring: [u16; VIRTQ_MAX_SIZE],
}

struct VirtqUsed {
    flags: u16,
    idx: AtomicU16,  // Device writes; driver reads
    ring: [VirtqUsedElem; VIRTQ_MAX_SIZE],
}

struct VirtqUsedElem {
    id: u32,
    len: u32,
}
```

### Memory Ordering & Volatile Access

Use `AtomicU16` with `Ordering::SeqCst` for `avail.idx` and `used.idx` to preserve sequential consistency with device:
- `avail->idx` write followed by doorbell must be visible to device
- `used->idx` read after polling must see latest device write
- Alternative: manual `std::sync::atomic::fence()` or inline ASM `dsb_sy()`

Descriptors and rings use `volatile` pointers or `core::ptr::read_volatile/write_volatile` where appropriate.

### Barrier Implementation

```rust
fn memory_barrier() {
    #[cfg(target_arch = "aarch64")]
    unsafe {
        core::arch::asm!("dsb sy");
    }
}
```

Or wrap the `utils::dsb_sy()` C function.

### Error Handling

```rust
pub enum ConsoleError {
    DeviceNotFound,
    HeaderTypeMismatch,
    FeatureNegotiationFailed,
    QueueSetupFailed(usize),  // Queue index
    NotReady,
    Timeout,
}

impl fmt::Display for ConsoleError { ... }
impl Error for ConsoleError { ... }
```

### Initialization Flow (Rust Equivalent)

```rust
pub fn init() -> Result<(), ConsoleError> {
    info!("[CONSOLE] Initializing Device");
    
    let pci_dev = pci::find_device(VENDOR_ID, DEVICE_ID)
        .ok_or(ConsoleError::DeviceNotFound)?;
    
    if pci_dev.header_type() & 0x7F != 0x00 {
        return Err(ConsoleError::HeaderTypeMismatch);
    }
    
    pci_dev.assign_bars();
    pci_dev.enable();
    let caps = virtio::parse_capabilities(&pci_dev)?;
    
    let mut console = Console::new(pci_dev, caps)?;
    console.reset_and_negotiate()?;
    console.setup_queues()?;
    
    let mut state = CONSOLE.lock();
    *state = Some(console);
    
    info!("[CONSOLE] DRIVER_OK; tx-only path live");
    vcons_send(b"[Fermi OS] virtio-console attached. Hello from guest!\n")?;
    
    Ok(())
}

pub fn send(buf: &[u8]) -> Result<usize, ConsoleError> {
    let state = CONSOLE.lock();
    let console = state.as_ref().ok_or(ConsoleError::NotReady)?;
    
    let mut sent = 0;
    while sent < buf.len() {
        let chunk_size = std::cmp::min(CONSOLE_TX_BUF, buf.len() - sent);
        console.tx_fragment(&buf[sent..sent + chunk_size])?;
        sent += chunk_size;
    }
    
    Ok(sent)
}

impl Console {
    fn tx_fragment(&self, chunk: &[u8]) -> Result<(), ConsoleError> {
        // Copy to DMA buffer
        TX_BUFFER.0[..chunk.len()].copy_from_slice(chunk);
        
        // Build descriptor
        let pa = virt_to_phys(TX_BUFFER.0.as_ptr() as u64);
        self.tx_vq.submit(pa, chunk.len() as u32, 0)?;
        self.tx_vq.notify();
        self.tx_vq.poll()?;
        
        Ok(())
    }
}
```

### Assembly Requirements (NOT Porting to Rust)

**Must stay in ASM (.S files):**
- `dsb_sy` instruction (or wrap in inline asm)
- PCI ECAM address computation (can be Rust, but traditionally inline)
- MMU register writes (already in separate mm module, can be called from console)

**Safe to port to Rust:**
- Device state machine (register writes/reads via MMIO)
- Descriptor ring management (Rust struct layout control)
- Fragmentation logic
- Error handling & logging

### No_std Compatibility

Console driver is bare-metal, so:
- Use `core::*` (not `std::*`)
- No allocator needed (all buffers static, VirtIO queues fixed-size)
- Use `#![no_std]` module attributes
- Provide `core::fmt::Write` impl if needed for logging convenience

## Testing Strategy (In Rust)

1. **Unit**: `Console::new()`, descriptor layout, address conversion
2. **Integration**: Boot-time init, verify ring memory at correct PA
3. **Functional**: `vcons_send()` with payloads 0, 1, 4096, 8192 bytes, check host file
4. **Stress**: Rapid sends, fragmented writes, concurrent shell `vlog` commands
5. **Error**: Device not present, feature negotiation failure (via mock), timeout simulation

## References

- VirtIO Specification 1.x, §3.0 (Device Init), §4.1.4 (PCI Transport), §5.3 (Console Device)
- ARM Architecture Reference Manual: ARMv8 (Data Synchronization Barriers)
- QEMU Virtio Console Device Model: `hw/char/virtio-console.c`
- Kernel Source: `src/pci/virtio/console/console.c`, `src/pci/virtio/virtqueue.c`
