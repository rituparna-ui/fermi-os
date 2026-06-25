# PCI Subsystem - C to Rust Port Reference

## Overview

The PCI subsystem implements ECAM (Enhanced Configuration Access Mechanism) based bus enumeration and BAR (Base Address Register) assignment for QEMU aarch64 virt machines. The subsystem performs brute-force enumeration of all buses/slots/functions, identifies devices, and allocates I/O mapped memory (MMIO) windows for both 32-bit and 64-bit memory BARs. Configuration space is accessed through ECAM physical addresses transformed to virtual via the MMU's upper-half kernel mapping (TTBR1).

The subsystem operates only during boot (from `kernel_main()` before task scheduling begins) and maintains a static cache of discovered devices. It has no runtime concurrency concerns.

### QEMU virt Machine PCI Layout (Physical Addresses)

From QEMU device tree, aarch64 virt machines define:
- **ECAM base**: `0x4010_0000_0000` (covers 256 buses)
- **PIO window**: `0x3eff_0000` (64 KiB) — NOT used in this implementation
- **32-bit MMIO**: `0x1000_0000` — `0x3efe_ffff` (510 MiB)
- **64-bit MMIO**: `0x8000_0000_00` — `0xffff_ffff_ff` (1 TiB)

All physical addresses are translated to kernel virtual addresses by the MMIO layer, which adds `KERNEL_VA_OFFSET` after `mmio_switch_to_upper()` is called.

## Public API

### Structures

#### `struct pci_device`

```c
struct pci_device {
  uint8_t bus;           // PCI bus number (0-255)
  uint8_t slot;          // device slot on bus (0-31, also called "device")
  uint8_t func;          // function within slot (0-7)
  uint16_t vendor_id;    // PCI vendor ID (e.g., 0x1af4 for Red Hat)
  uint16_t device_id;    // PCI device ID
  uintptr_t bar_addr[6]; // Physical addresses of 6 BARs after assignment
};
```

**Layout & Semantics**:
- Bus, slot, func form the PCI triplet (bus:slot.func)
- Each BAR is 32-bit aligned; only populated BAR indices contain valid addresses
- 64-bit BAR occupies two consecutive BAR slots; upper half stored in `bar_addr[i+1]`
- Implementation does NOT support I/O space BARs (bit 0 = 0); memory space only

### Enumeration & Discovery

#### `void pci_enumerate_bus(void)`

Brute-force scan of all buses, slots, and functions.

**Algorithm**:
1. Iterate bus 0..255, slot 0..31, func 0..7
2. For each triplet, read vendor ID at offset `0x00`
3. Skip if vendor ID == `0xFFFF` (no device, unimplemented slot)
4. Otherwise: read device ID at offset `0x02`, log device, cache in global array
5. Stop if cache reaches `MAX_PCI_DEVICES` (16 devices)

**Logging**: Each found device prints `[PCI] Device found at B:S.F | VendorID: XXXX, DeviceID: XXXX`

**State Modified**:
- `pci_devices[]` array populated with discovered devices
- `pci_device_count` incremented

**Dependencies**: UART (for logging), MMIO layer (for config space access)

#### `int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *pci_device)`

Linear search of cached devices by vendor and device ID.

**Returns**:
- `ESUCCESS` (1) if exact match found; `*pci_device` filled
- `EERROR` (0) if not found

**Parameters**:
- `vendor_id`, `device_id`: exact match criteria
- `pci_device`: output; callee writes device record on success

**O(n)** linear search; safe only after `pci_enumerate_bus()` completed.

### Configuration Space Access

All configuration space reads/writes use ECAM addressing. The MMIO layer adds the kernel VA offset.

#### `uint32_t pci_config_read32(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset)`
#### `uint16_t pci_config_read16(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset)`
#### `uint8_t pci_config_read8(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset)`

Reads 32-bit, 16-bit, or 8-bit values from PCI config space.

**ECAM address formula**:
```
ecam_phys = PCI_ECAM_PHYS | (bus << 20) | (slot << 15) | (func << 12) | offset
```

where:
- `PCI_ECAM_PHYS = 0x4010_0000_0000`
- `offset` is the register offset within config space (0-255)

**Parameters**:
- `bus`: PCI bus number (0-255)
- `slot`: device slot (0-31)
- `func`: function (0-7)
- `offset`: config space byte offset (e.g., `0x00` for vendor ID)

**Dependencies**: MMIO layer (`mmio_read32`, `mmio_read16`, `mmio_read8`)

#### `void pci_config_write32(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val)`
#### `void pci_config_write16(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t val)`
#### `void pci_config_write8(uint16_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t val)`

Writes 32-bit, 16-bit, or 8-bit values to PCI config space. Same ECAM addressing scheme.

**Dependencies**: MMIO layer (`mmio_write32`, `mmio_write16`, `mmio_write8`)

### Device Utilities

#### `uint8_t pci_get_header_type(struct pci_device *dev)`

Reads the header type byte from config space offset `0x0E`.

**Returns**: Raw header type byte. Standard PCI header types:
- `0x00`: endpoint device
- `0x01`: PCI-to-PCI bridge
- `0x02`: PCI-to-CardBus bridge

**Current Implementation**: Not actually used in the C code; provided for future subsystem callers.

### BAR Assignment

#### `void pci_assign_bars(struct pci_device *dev)`

Discovers and assigns physical addresses to all 6 BARs of a device.

**Algorithm per BAR**:
1. Read BAR configuration value at offset `PCI_BAR0 + i*4`
2. Check bit 0: if set, I/O space BAR — skip
3. Memory space BARs: extract type field (bits 2-1):
   - `0x00`: 32-bit BAR
     - Probe size by writing `0xFFFFFFFF`, reading back, restoring original
     - Size = `~(mask & ~0xF) + 1` (mask out control bits 3-0)
     - Allocate from 32-bit MMIO window via `alloc_mmio32()`
     - Write allocated address back to BAR register
   - `0x02`: 64-bit BAR (occupies `BAR[i]` and `BAR[i+1]`)
     - If `i + 1 >= 6`, error (no upper half for BAR5)
     - Probe both halves together via `pci_get_bar_size64()`
     - Allocate from 64-bit MMIO window via `alloc_mmio64()`
     - Write lower 32 bits to `BAR[i]`, upper 32 bits to `BAR[i+1]`
     - Increment loop counter `i++` to skip upper BAR
   - Other values: unsupported, error logged

**Side Effects**:
- Modifies device's BAR registers in PCI config space
- Fills `dev->bar_addr[0..5]` with allocated physical addresses (0 if unimplemented)
- Updates global MMIO allocators (`mmio32_next`, `mmio64_next`)
- Logs each BAR size and assignment

**Error Handling**:
- MMIO window exhaustion: logs error, returns 0 from allocator
- Invalid 64-bit BAR at slot 5: logs error, continues

#### `uint32_t pci_get_bar_size(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset)` (static)

Probes 32-bit BAR size by write-1s-read-back.

**Algorithm**:
1. Read current BAR value (saved for restoration)
2. Write `0xFFFFFFFF` to BAR
3. Read back result (size bits now set to 1s)
4. Restore original value
5. Mask off control bits (3-0): `size_mask &= ~0xF`
6. Compute size: `size = ~size_mask + 1`
7. Return 0 if unimplemented BAR (size_mask == 0)

**Return Value**: Aligned size in bytes; 0 if unimplemented

#### `uint64_t pci_get_bar_size64(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset_lo, uint16_t offset_hi)` (static)

Probes 64-bit BAR size by simultaneous write-1s-read-back to both halves.

**Algorithm**:
1. Save both 32-bit BAR registers (low and high)
2. Write `0xFFFFFFFF` to both
3. Read back both (size bits now set)
4. Restore both original values
5. Construct 64-bit mask: `mask = ((mask_hi << 32) | (mask_lo & ~0xFUL))`
   - Note: only low word has control bits 3-0 cleared
6. Compute size: `size = ~mask + 1`

**Return Value**: Aligned 64-bit size in bytes

### Device Enablement

#### `void pci_enable_device(struct pci_device *dev)`

Sets command register flags to enable memory access and DMA.

**Algorithm**:
1. Read command register at offset `0x04`
2. Set bit 1: Memory Space Enable
3. Set bit 2: Bus Master Enable (DMA)
4. Write updated command register

**Effect**: Device can now respond to MMIO requests at assigned BAR addresses and perform DMA

**Logging**: Prints `[PCI] Enabling device` and `[PCI] Device Enabled`

## Hardware Constants & Register Offsets

### PCI Config Space Offsets

| Offset | Width | Name | Purpose |
|--------|-------|------|---------|
| `0x00` | 16-bit | Vendor ID | Device manufacturer |
| `0x02` | 16-bit | Device ID | Device model |
| `0x04` | 16-bit | Command | Enable/disable features (see bits below) |
| `0x06` | 16-bit | Status | Device status flags |
| `0x0E` | 8-bit | Header Type | Endpoint (0x00), bridge (0x01), CardBus (0x02) |
| `0x10` | 32-bit | BAR0 | First base address register |
| `0x14` | 32-bit | BAR1 | Second base address register |
| `0x18` | 32-bit | BAR2 | Third base address register |
| `0x1C` | 32-bit | BAR3 | Fourth base address register |
| `0x20` | 32-bit | BAR4 | Fifth base address register |
| `0x24` | 32-bit | BAR5 | Sixth base address register |
| `0x34` | 8-bit | Capability Pointer | Points to capability list (not used) |

### Command Register (0x04)

| Bit | Name | Purpose |
|-----|------|---------|
| 0 | I/O Space Enable | Allow I/O BAR access |
| 1 | Memory Space Enable | Allow memory BAR access |
| 2 | Bus Master | Enable DMA |
| 3-5 | Reserved | |
| 6 | SERR# Enable | Enable error reporting |
| 7-15 | Reserved | |

**This subsystem sets**: bits 1 and 2 (Memory + DMA)

### BAR Layout (Memory Space)

**32-bit BAR (bit 2-1 = 0x00)**:
```
31                                                        4 3     1 0
[.................. Address[31:4] ..........................][Type][0]
                                                            10      1
```

- Bit 0: always 0 for memory space BARs
- Bits 2-1: memory type
  - `00` = 32-bit BAR
  - `10` = 64-bit BAR
  - `01`, `11` = reserved
- Bits 3-4: prefetchable (cacheable) flag
- Bits 31-5: physical address (naturally aligned to size)

**64-bit BAR (bit 2-1 = 0x10)**:
```
BAR[i]:     [.................. Address[31:4] ..........................][10][0]
BAR[i+1]:   [.......................... Address[63:32] ..............................] 
```

- BAR[i] holds low 32 bits of 64-bit address; bits 3-0 ignored (control bits)
- BAR[i+1] holds high 32 bits
- Size probed across both registers simultaneously

### ECAM Addressing

ECAM physical address construction (for bus `B`, slot `S`, function `F`, config offset `OFF`):

```
ecam_addr = PCI_ECAM_PHYS | (B << 20) | (S << 15) | (F << 12) | OFF
          = 0x4010_0000_0000 | (B << 20) | (S << 15) | (F << 12) | OFF
```

**Example**: Bus 0, Slot 2, Func 0, Vendor ID:
```
ecam_addr = 0x4010_0000_0000 | 0x0000_0000 | (2 << 15) | 0 | 0x00
          = 0x4010_0000_8000
```

MMIO layer translates this physical address to kernel virtual via upper-half mapping.

### MMIO Allocator Constants

```
PCI_MMIO32_PHYS = 0x1000_0000UL    // Start of 32-bit MMIO window
PCI_MMIO32_LIMIT = 0x3EFE_FFFFUL   // End of 32-bit MMIO window (510 MiB total)
PCI_MMIO64_PHYS = 0x8000_0000_00UL // Start of 64-bit MMIO window
PCI_MMIO64_LIMIT = 0xFFFF_FFFF_FFUL // End of 64-bit MMIO window (1 TiB total)
```

### Enumeration Limits

| Constant | Value | Purpose |
|----------|-------|---------|
| `MAX_PCI_DEVICES` | 16 | Static cache size |
| `MAX_PCI_BUS` | 256 | Enumeration range |
| `MAX_PCI_SLOT` | 32 | Enumeration range |
| `MAX_PCI_FUNC` | 8 | Enumeration range |

## Boot Sequence & Usage Ordering

### Initialization Order (from `kernel_main()`)

```c
pci_enumerate_bus();           // 1. Discover all devices
pci_virtio_rng_init();          // 2. Initialize RNG device (calls pci_find_device, pci_assign_bars, pci_enable_device)
pci_virtio_blk_init();          // 3. Initialize block device
pci_virtio_net_init();          // 4. Initialize network device
pci_virtio_balloon_init();      // 5. Initialize balloon device
pci_virtio_console_init();      // 6. Initialize console device
```

### Critical Constraints

1. **MMIO Mapping Must Be Active**: All config space access requires upper-half kernel mapping. Callers must run after `mmio_switch_to_upper()`.

2. **Single-Phase Enumeration**: `pci_enumerate_bus()` is called once, before any device initialization. No dynamic device hotplug.

3. **BAR Assignment Before Enable**: Must call `pci_assign_bars()` before `pci_enable_device()` for each device. Configuration space is sticky after assignment.

4. **Sequential Device Drivers**: Virtio subsystems (`pci_virtio_*_init()`) call PCI API sequentially to find and configure each device. No parallelism at boot time.

5. **No Teardown**: Devices are never disabled or reconfigured after boot.

## Implementation Details

### Static State

```c
static struct pci_device pci_devices[MAX_PCI_DEVICES];  // Cache of discovered devices
static uint16_t pci_device_count = 0;                   // Count of cached devices

static uintptr_t mmio32_next = PCI_MMIO32_PHYS;         // Allocator cursor (32-bit window)
static uintptr_t mmio64_next = PCI_MMIO64_PHYS;         // Allocator cursor (64-bit window)
```

### Natural Alignment During Allocation

The allocators implement natural alignment (power-of-2 alignment matching the requested size):

```c
static uintptr_t alloc_mmio32(uint32_t size) {
  uintptr_t mask = (uintptr_t)size - 1;
  mmio32_next = (mmio32_next + mask) & ~mask;  // Align up
  if (mmio32_next + size - 1 > PCI_MMIO32_LIMIT) {
    uart_errorln("[PCI] 32-bit MMIO window exhausted");
    return 0;
  }
  uintptr_t addr = mmio32_next;
  mmio32_next += size;
  return addr;
}
```

This ensures BAR addresses are naturally aligned, satisfying PCI spec requirements.

### Write-1s-Read-Back BAR Size Probing

PCI devices implement size probing by temporarily writing all-1s to the address bits and reading back which bits are writable:

1. Device firmware masks lower bits (I/O type, prefetch flags) as read-only
2. Only address bits are writable
3. Reading back shows which bits are implemented
4. Size = 2^(position of lowest set bit after write-1s)

This is safe because:
- Config space writes are locked to root/bootloader after POST
- Devices are disabled during this phase (memory space enable bit not set)
- No DMA in progress

## Rust Port Strategy

### Module Structure

```rust
// src/pci/mod.rs
pub mod config;      // ECAM config space access (reads/writes)
pub mod enumerate;   // Bus enumeration & device discovery
pub mod bars;        // BAR size probing & allocation
pub mod device;      // pci_device struct & utilities

pub use config::{read32, read16, read8, write32, write16, write8};
pub use enumerate::{enumerate_bus, find_device};
pub use bars::assign_bars;
pub use device::{enable_device, Device, get_header_type};
```

### Core Types

```rust
#[derive(Clone, Copy)]
pub struct Device {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub bar_addr: [usize; 6],
}

#[derive(Clone, Copy)]
pub enum Result<T> {
    Success(T),
    Error(&'static str),
}
pub use Result::{Success as Ok, Error as Err};
```

### Static State Management

```rust
static DEVICES: spin::Mutex<[Option<Device>; MAX_PCI_DEVICES]> = spin::Mutex::new([None; 16]);
static DEVICE_COUNT: core::sync::atomic::AtomicU16 = core::sync::atomic::AtomicU16::new(0);

// MMIO allocators (incremented sequentially)
static MMIO32_NEXT: core::sync::atomic::AtomicUsize = 
    core::sync::atomic::AtomicUsize::new(PCI_MMIO32_PHYS);
static MMIO64_NEXT: core::sync::atomic::AtomicUsize = 
    core::sync::atomic::AtomicUsize::new(PCI_MMIO64_PHYS);
```

**Note**: Boot phase is single-threaded; use `Mutex` + `AtomicUsize` for correctness even though contention is impossible, to avoid `unsafe` during device cache updates.

### Key Ownership Model

- **Device discovery**: `enumerate_bus()` fills cache; callers receive `&Device` references
- **BAR allocation**: MMIO allocators are "bump" allocators (only increase); use `AtomicUsize::fetch_add()` for thread-safe (but unnecessary) increments
- **Configuration**: `assign_bars()` modifies both device struct and PCI config space atomically

### MMIO Access Layer

Retain MMIO layer abstraction:
```rust
use crate::mmio::{read32, read16, read8, write32, write16, write8};
// or if consolidating:
mod mmio {
    pub fn read32(addr: usize) -> u32 { ... }
    pub fn write32(addr: usize, val: u32) { ... }
    // etc.
}
```

Configuration space address formula:
```rust
fn make_ecam_addr(bus: u16, slot: u8, func: u8, offset: u16) -> usize {
    PCI_ECAM_PHYS | ((bus as usize) << 20) 
                  | ((slot as usize) << 15) 
                  | ((func as usize) << 12) 
                  | (offset as usize)
}
```

### Error Handling

Use explicit `Result` type (or Rust's built-in `Result`) rather than C-style integer returns:

```rust
pub fn find_device(vendor_id: u16, device_id: u16) -> Option<Device> {
    let devices = DEVICES.lock();
    for i in 0..(*DEVICE_COUNT.load(Ordering::Acquire) as usize) {
        if let Some(dev) = devices[i] {
            if dev.vendor_id == vendor_id && dev.device_id == device_id {
                return Some(dev);
            }
        }
    }
    None
}
```

### Logging Integration

Retain UART logging calls:
```rust
use crate::uart::println;

pub fn enumerate_bus() {
    println!("[PCI] Enumerating PCI Devices");
    // ...
}
```

## Architecture Decisions & Gotchas

### Gotchas

1. **Control Bit Masking**: BAR size probing masks bits 3-0 of the result: `size_mask &= ~0xF`. Failure to mask correctly yields wrong sizes.

2. **64-bit BAR High Bits**: `pci_get_bar_size64()` does NOT mask bits 3-0 of the high word (which should be zero), only of the low word. Mask formula: `mask = ((mask_hi << 32) | (mask_lo & ~0xFUL))`.

3. **Loop Counter Increment**: After assigning a 64-bit BAR at index `i`, the loop must increment `i++` to skip `BAR[i+1]` (which was just filled with the upper half). Forgetting this causes the upper half to be reinterpreted as a new 32-bit BAR and allocated wrongly.

4. **Natural Alignment**: Allocators must align to power-of-2 size boundaries. The formula `mask = size - 1; addr = (addr + mask) & ~mask` works only when size is a power of 2. Most PCI devices report power-of-2 sizes, but corner cases exist.

5. **Volatile MMIO Access**: Reads/writes must not be optimized away. MMIO layer must emit actual memory operations. In Rust, use `volatile::*` or inline assembly.

6. **ECAM Bit Offsets**: The ECAM addressing scheme packs bus/slot/func into the upper bits of the address:
   - Bus at bit 20 (256 buses × 1 MiB each)
   - Slot at bit 15 (32 slots × 32 KiB each)
   - Func at bit 12 (8 functions × 4 KiB each)
   Off-by-one errors in shift amounts break addressing.

7. **No Multi-Function Enumeration Shortcut**: The code does not use the PCI multi-function device bit to skip unimplemented functions on single-function devices. It brute-forces all 2048 (bus × slot × func) triplets. Optimization is possible but not necessary for boot time.

8. **BAR5 Cannot Be 64-bit**: BAR5 is the last register; a 64-bit BAR would need a non-existent BAR6. Code checks `if (i + 1 >= 6)` and errors. Edge case but critical.

9. **MMIO Window Exhaustion**: If too many devices or large BARs are present, the allocator may run out of address space. Current code logs an error and returns 0, which is then written to the BAR. This leaves the device unconfigurable. Crash or graceful degradation is implementation-dependent.

10. **Vendor/Device ID == 0xFFFF**: Used as a sentinel for "no device". Real devices can have any vendor ID except this, but the code assumes it never appears in QEMU virt machines.

### Volatility & Barriers

- All config space access must be volatile (not elided or reordered by compiler)
- MMIO layer must enforce this via volatile reads/writes or `volatile` keyword
- No explicit barriers needed at boot time (sequential initialization)

### Hardware Specificity

- **QEMU virt only**: Constants (`PCI_ECAM_PHYS`, `PCI_MMIO32_*`, etc.) are hardcoded for QEMU aarch64 virt
- **No ACPI/device tree parsing**: Addresses must match QEMU device tree or system will hang/crash
- **No PCI-to-PCI bridges**: Enumeration assumes all devices on bus 0 (no secondary buses)

## Dependencies

### Inbound (who calls PCI)

- **kernel.c**: `kernel_main()` calls `pci_enumerate_bus()`, then each virtio subsystem calls `pci_find_device()`, `pci_assign_bars()`, `pci_enable_device()`
- **pci/virtio/rng**: Finds RNG device, assigns BARs, enables
- **pci/virtio/blk**: Finds block device, assigns BARs, enables
- **pci/virtio/net**: Finds network device, assigns BARs, enables
- **pci/virtio/balloon**: Finds balloon device, assigns BARs, enables
- **pci/virtio/console**: Finds console device, assigns BARs, enables

### Outbound (PCI calls)

- **mmio**: `mmio_read32/16/8()`, `mmio_write32/16/8()` for all config space access
- **uart**: `uart_println()`, `uart_errorln()`, `uart_printf()` for logging
- **utils**: `ESUCCESS`, `EERROR` constants

### No Direct Dependencies

- Does NOT call malloc/alloc (uses static device cache)
- Does NOT call MMU code
- Does NOT interact with timers, interrupts, or schedulers
- Does NOT touch UART config (assumes `uart_init()` already called)

## Summary Table

| Aspect | Detail |
|--------|--------|
| **Purpose** | ECAM-based PCI bus enumeration, device discovery, BAR allocation |
| **Entry Points** | `pci_enumerate_bus()`, `pci_find_device()`, `pci_assign_bars()`, `pci_enable_device()` |
| **Static Cache** | 16 devices, filled once at boot, never evicted |
| **MMIO Allocation** | Two bump allocators (32-bit: 510 MiB, 64-bit: 1 TiB) |
| **Concurrency** | None (boot phase only, single-threaded) |
| **Memory Safety** | Rust: use Mutex for device cache, AtomicUsize for allocators |
| **Volatility** | All config space reads/writes must be volatile |
| **Porting Effort** | Low–medium: straightforward logic, no concurrency, clear error cases |
| **Test Coverage** | Manual: enumerate devices on QEMU virt, verify BAR addresses assigned correctly |

