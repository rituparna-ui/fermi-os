# CPU Subsystem Specification

## Overview

The CPU subsystem provides:
1. **CPU identification**: Read ARMv8-A system registers to determine processor type, architecture revision, and feature support
2. **Feature detection**: Decode and expose FP, AdvSIMD, AES, SHA1, SHA2, CRC32, RNDR capabilities
3. **Cache topology**: Report instruction and data cache line sizes
4. **Memory model**: Decode physical address range capabilities
5. **PMU cycle counter**: Enable and read the 64-bit performance monitor cycle counter (PMCCNTR_EL0)

All operations use architecturally-defined EL1 sysregisters — no hardware-specific assumptions beyond standard ARMv8-A.

### Module Hierarchy
- `cpu_init()` — called once during early kernel boot (from `kernel_main`)
- `cpu_read_cycles()` — safe from any context after `cpu_init()`
- `cpu_render_info()` — safe from any context after `cpu_init()`

---

## Public API

### void cpu_init(void)
**Preconditions:**
- Must be called from EL1 (exception level 1)
- May be called only once; subsequent calls are harmless (re-read and re-configure registers)

**Behavior:**
1. Read and cache identification registers (MIDR_EL1, CTR_EL0, ID_AA64PFR0_EL1, ID_AA64ISAR0_EL1, ID_AA64MMFR0_EL1)
2. Configure and enable the PMU cycle counter:
   - Write PMCR_EL0 with flags: E, P, C, LC set
   - Write PMCNTENSET_EL0[31] to enable dedicated cycle counter
3. Print boot message to UART with implementer name, part name, revision, MIDR value

**Postconditions:**
- All cached registers are populated
- PMU cycle counter is running and will increment every cycle
- PMCCNTR_EL0 reads return cycle count (not guaranteed to start at 0 due to reset timing)

**Side Effects:**
- Modifies PMCR_EL0 and PMCNTENSET_EL0
- Calls `uart_printf()` twice with boot messages
- Populates static globals: `g_midr`, `g_ctr`, `g_pfr0`, `g_isar0`, `g_mmfr0`

---

### uint64_t cpu_read_cycles(void)
**Preconditions:**
- `cpu_init()` must have been called
- No exception level restriction; can be called from EL0 or EL1 (after `cpu_init()` enables the counter)

**Returns:**
- Current value of PMCCNTR_EL0 (64-bit cycle counter)
- Monotonically non-decreasing
- Wraps at 2^64

**Behavior:**
- Simple MRS read of PMCCNTR_EL0
- No side effects

---

### int cpu_render_info(char *buf, size_t len)
**Preconditions:**
- `cpu_init()` must have been called
- `buf` must be valid and writable for at least `len` bytes
- `len` should be at least 512 bytes to avoid truncation

**Returns:**
- Number of bytes written (excluding NUL terminator) if fits
- Number of bytes that would have been written (>= len) if truncated (POSIX snprintf semantics)
- Caller must check `return >= len` to detect truncation

**Behavior:**
- Formats CPU identification and feature information into a multi-line text output
- Decodes all cached sysregs into human-readable fields
- Calls `cpu_read_cycles()` and includes current cycle count in output
- Output format (always 17 lines):
  ```
  implementer  : <name> (0x<hex>)
  part         : <name> (0x<hex>)
  architecture : ARMv8 (0x<hex>)
  variant      : 0x<hex>
  revision     : 0x<hex>
  midr_el1     : <hex>
  icache_line  : <dec> bytes
  dcache_line  : <dec> bytes
  phys_addr    : <dec> bits
  fp           : yes|no
  advsimd      : yes|no
  aes          : yes|no
  sha1         : yes|no
  sha2         : yes|no
  crc32        : yes|no
  rndr         : yes|no
  cycles       : <dec>
  ```

**Side Effects:**
- Calls `cpu_read_cycles()` once
- Calls `ksnprintf()` once

---

## Hardware Constants and Register Layouts

### System Registers (ARMv8-A standard)

All registers are 64-bit (uint64_t). Register names are symbolic; the MRS/MSR macros handle encoding.

#### MIDR_EL1 — Main ID Register
**Read-only**. Identifies the processor.

| Bits  | Field      | Description |
|-------|-----------|-------------|
| 31:24 | Implementer | CPU manufacturer code (see table below) |
| 23:20 | Variant    | Major revision/stepping |
| 19:16 | Architecture | ARMv8-A = 0xF (version 8.0 baseline) |
| 15:4  | Part Number | Specific CPU model (for ARM Ltd: see table below) |
| 3:0   | Revision   | Minor stepping/patch revision |

**Implementer Codes** (8-bit, bits 31:24):
| Code | Manufacturer |
|------|--------------|
| 0x41 | ARM Limited |
| 0x42 | Broadcom |
| 0x43 | Cavium |
| 0x46 | Fujitsu |
| 0x48 | HiSilicon |
| 0x49 | Infineon |
| 0x4D | Motorola/Freescale |
| 0x4E | NVIDIA |
| 0x50 | Applied Micro |
| 0x51 | Qualcomm |
| 0x53 | Samsung |
| 0x54 | Texas Instruments |
| 0x56 | Marvell |
| 0x61 | Apple |
| 0x66 | Faraday |
| 0x69 | Intel |
| 0xC0 | Ampere |

**ARM Ltd Part Numbers** (12-bit, bits 15:4, when Implementer=0x41):
| Part   | CPU Model |
|--------|-----------|
| 0xD03  | Cortex-A53 |
| 0xD05  | Cortex-A55 |
| 0xD07  | Cortex-A57 |
| 0xD08  | Cortex-A72 |
| 0xD09  | Cortex-A73 |
| 0xD0A  | Cortex-A75 |
| 0xD0B  | Cortex-A76 |
| 0xD0D  | Cortex-A77 |
| 0xD40  | Neoverse-V1 |
| 0xD41  | Cortex-A78 |
| 0xD49  | Neoverse-N2 |
| 0xD4A  | Neoverse-E1 |

---

#### CTR_EL0 — Cache Type Register
**Read-only**. Describes cache topology.

| Bits  | Field | Description |
|-------|-------|-------------|
| 19:16 | DMinLine | Data cache minimum line size = 2^(DMinLine+2) words = 2^(DMinLine+4) bytes |
| 3:0   | IMinLine | Instruction cache minimum line size = 2^(IMinLine+2) words = 2^(IMinLine+4) bytes |

**Decoding (both fields):**
- Value is log2 of cache line size in **words** (4-byte units)
- Byte size = `(1 << field_value) * 4`
- Examples:
  - Field=2: 2^2=4 words → 16 bytes
  - Field=3: 2^3=8 words → 32 bytes
  - Field=4: 2^4=16 words → 64 bytes

---

#### ID_AA64PFR0_EL1 — AArch64 Processor Feature Register 0
**Read-only**. Indicates support for floating-point and SIMD extensions.

| Bits  | Field | Encoding |
|-------|-------|----------|
| 23:20 | AdvSIMD | `0x0`=supported, `0x1`=+half-precision, `0xF`=not supported |
| 19:16 | FP | `0x0`=supported, `0x1`=+half-precision, `0xF`=not supported |

**Decoding:**
- If field != 0xF: feature present
- If field == 0xF: feature absent
- 0x0 vs 0x1 distinction not used by kernel (both treated as "yes")

---

#### ID_AA64ISAR0_EL1 — AArch64 ISA Feature Register 0
**Read-only**. Indicates support for cryptographic and other instructions.

| Bits  | Field | Encoding (all 4-bit) |
|-------|-------|----------|
| 63:60 | RNDR | `0x0`=no, any other=supported (random number generator) |
| 19:16 | CRC32 | `0x0`=no, any other=supported |
| 15:12 | SHA2 | `0x0`=no, any other=supported |
| 11:8  | SHA1 | `0x0`=no, any other=supported |
| 7:4   | AES | `0x0`=no, any other=supported |

**Decoding:**
- For each field: if `(field & 0xF) != 0`, feature is present

---

#### ID_AA64MMFR0_EL1 — AArch64 Memory Model Feature Register 0
**Read-only**. Indicates physical address range and memory management capabilities.

| Bits | Field | Description |
|------|-------|-------------|
| 3:0  | PARange | Physical Address Range code (see table below) |

**PARange Values** (4-bit code → physical address bits):
| Code | Bits | Example |
|------|------|---------|
| 0    | 32   | 4 GB |
| 1    | 36   | 64 GB |
| 2    | 40   | 1 TB |
| 3    | 42   | 4 TB |
| 4    | 44   | 16 TB |
| 5    | 48   | 256 TB |
| 6    | 52   | 4 PB |
| other| 0    | invalid/reserved |

---

#### PMCR_EL0 — Performance Monitor Control Register
**Read/Write**. Controls PMU operation.

| Bits | Field | Purpose |
|------|-------|---------|
| 6    | LC | Long Counter mode: 1=64-bit cycle counter (no overflow at 2^32), 0=32-bit |
| 2    | C | Reset cycle counter: write 1 to zero PMCCNTR_EL0 |
| 1    | P | Reset all event counters: write 1 to zero all event counters |
| 0    | E | Enable bit: 1=PMU active, 0=PMU disabled |

**Boot Initialization:**
- Write `(1ULL << 6) | (1ULL << 2) | (1ULL << 1) | (1ULL << 0)` = `0x47` to enable PMU, reset counters, and set 64-bit mode

---

#### PMCNTENSET_EL0 — Performance Monitor Count Enable Set
**Read/Write**. Enable individual PMU counters.

| Bits | Description |
|------|-------------|
| 31   | Cycle counter enable: 1=enable PMCCNTR_EL0 |
| 30:0 | Event counter enables (not used by kernel) |

**Boot Initialization:**
- Write `(1ULL << 31)` to enable the dedicated cycle counter

---

#### PMCCNTR_EL0 — Performance Monitor Cycle Counter
**Read/Write** (though typically only read after boot). 64-bit cycle counter.

- Increments once per CPU cycle
- Wraps at 2^64 (never in practice for typical workloads)
- Frozen when PMU disabled or when this counter not enabled in PMCNTENSET_EL0

---

## Static Module State

### Cached CPU Identification (file-static globals)

All initialized by `cpu_init()`, then never modified.

```c
static uint64_t g_midr;    // Main ID Register snapshot
static uint64_t g_ctr;     // Cache Type Register snapshot
static uint64_t g_pfr0;    // Processor Feature Register 0 snapshot
static uint64_t g_isar0;   // ISA Feature Register 0 snapshot
static uint64_t g_mmfr0;   // Memory Model Feature Register 0 snapshot
```

**Rationale:** These registers do not change at runtime; caching avoids repeated MRS instructions and ensures consistent reporting across calls.

---

## Internal Helper Functions

### static const char *implementer_name(uint8_t imp)
**Purpose:** Convert MIDR_EL1[31:24] to human-readable manufacturer name.

**Input:** 8-bit implementer code

**Returns:** Pointer to static string (never NULL)
- Known implementers return their name (e.g., "ARM Limited", "NVIDIA")
- Unknown codes return "Unknown"

**Side Effects:** None (pure function, static data only)

---

### static const char *arm_part_name(uint16_t part)
**Purpose:** Convert ARM Ltd part number to CPU model name (e.g., 0xD08 → "Cortex-A72").

**Input:** 12-bit part number

**Returns:** Pointer to static string (never NULL)
- Known ARM part numbers return model name (e.g., "Cortex-A75")
- Unknown codes return "unknown"

**Side Effects:** None (pure function, static data only)

**Note:** Only meaningful when Implementer=0x41 (ARM Limited). Called from `cpu_init()` conditionally.

---

### static uint64_t parange_bits(uint64_t mmfr0)
**Purpose:** Decode PARange field of ID_AA64MMFR0_EL1 to physical address width in bits.

**Input:** ID_AA64MMFR0_EL1 value (full 64-bit register)

**Returns:** Physical address width in bits
- Valid codes (0-6) → 32, 36, 40, 42, 44, 48, 52
- Invalid/reserved codes → 0

**Behavior:** Extract bits [3:0], switch on value

**Side Effects:** None (pure function)

---

## Assembly and Register Access

The subsystem uses inline assembly through two macros, defined in cpu.c:

```c
#define MRS(reg)                                                               
  ({                                                                           
    uint64_t _v;                                                               
    __asm__ __volatile__("mrs %0, " #reg : "=r"(_v));                          
    _v;                                                                        
  })

#define MSR(reg, val)                                                          
  do {                                                                         
    uint64_t _v = (val);                                                       
    __asm__ __volatile__("msr " #reg ", %0" ::"r"(_v));                        
  } while (0)
```

**MRS(reg):** Move Register from System register → uint64_t variable
- Expands to inline asm instruction `mrs %0, <reg>`
- Returns the 64-bit value read from the sysreg

**MSR(reg, val):** Move Register to System register from uint64_t variable
- Expands to inline asm instruction `msr <reg>, %0`
- Writes the 64-bit value to the sysreg

**Volatility:** Both marked `__volatile__` to ensure no optimization/reordering.

---

## Boot Sequence and Dependencies

### Ordering
1. **Early boot (EL1 PAS):** `enable_fp_simd()` must be called before `cpu_init()` (though not enforced)
2. **Early boot (EL1 SAS):** `uart_init()` must be called before `cpu_init()` (uart_printf called during init)
3. **Call site:** `cpu_init()` called from `kernel_main()` after MMU enable but before heap initialization

### Call Graph (from kernel.c)
```
kernel_main()
├─ early_init()
│  ├─ uart_init()
│  └─ enable_fp_simd()
├─ cpu_init()  ← CPU subsystem entry point
├─ heap_init()
├─ gic_init()
└─ ...other subsystems
```

### Dependencies
- **Input (called by):** kernel.c `kernel_main()` (once at boot)
- **Output (calls):**
  - uart.h: `uart_printf()` (for boot messages)
  - strings.h: `ksnprintf()` (for `cpu_render_info()`)
- **Hardware:** EL1 sysreg access (no MMIO, no interrupts needed)
- **No mutual dependencies with other kernel subsystems** (CPU state is read-only after boot)

---

## Rust Port Strategy

### Module Structure
```rust
pub mod cpu {
    // src/lib/cpu/mod.rs
    
    pub use self::sysreg::*;
    pub use self::pmu::*;
    pub use self::decode::*;
    
    mod sysreg;   // Inline asm wrappers for MRS/MSR
    mod pmu;      // PMU enable logic
    mod decode;   // Register field extraction, lookups, formatting
    
    pub fn init() { ... }
    pub fn read_cycles() -> u64 { ... }
    pub fn render_info(buf: &mut [u8]) -> usize { ... }
}
```

### Types
```rust
#[derive(Debug, Clone, Copy)]
pub struct CpuInfo {
    pub midr:   u64,
    pub ctr:    u64,
    pub pfr0:   u64,
    pub isar0:  u64,
    pub mmfr0:  u64,
}

pub struct CpuIdent {
    pub implementer:  u8,
    pub variant:      u8,
    pub arch:         u8,
    pub part_number:  u16,
    pub revision:     u8,
}

pub struct CpuFeatures {
    pub fp:      bool,
    pub advsimd: bool,
    pub aes:     bool,
    pub sha1:    bool,
    pub sha2:    bool,
    pub crc32:   bool,
    pub rndr:    bool,
}

pub struct CacheInfo {
    pub icache_line_bytes: u64,
    pub dcache_line_bytes: u64,
}

pub struct MemoryModel {
    pub phys_addr_bits: u64,
}
```

### Static Storage
```rust
// Cached CPU identification read at boot, immutable thereafter
static CPU_INFO: OnceCell<CpuInfo> = OnceCell::new();

// Alternative: use a Mutex if RwLock is overkill
// static CPU_INFO: Mutex<Option<CpuInfo>> = Mutex::new(None);
```

### Key Implementation Notes
1. **MRS/MSR macros → inline assembly functions** in `sysreg.rs` — each sysreg gets a dedicated Rust function wrapping the asm
2. **No macro expansion** — explicitly write out each sysreg function (better for documentation, type safety)
3. **Locking:** OnceCell or LazyLock (no runtime locks; cold init only)
4. **Endianness:** Native (ARM is always little-endian at EL1 in ARMv8)
5. **Bit extraction:** Use bitfield crate or manual shifts—C code uses manual, port as-is
6. **String formatting:** Use `core::fmt` or `ksnprintf` wrapper (already ported)

### Assembly Must-Stay
- **MRS/MSR instructions:** Must use inline asm (only way to access sysregs from Rust)
- **Macros → functions:** No dynamic register access; each sysreg gets its own function
- Everything else can be pure Rust (bit extraction, lookups, formatting)

### Call Compatibility
- `init()` → `cpu_init()` (no args, no return)
- `read_cycles() -> u64` → `cpu_read_cycles() -> uint64_t`
- `render_info(buf: &mut [u8]) -> usize` → `cpu_render_info(char *buf, size_t len) -> int` (convert &mut [u8] to C pointer/len)

---

## Exact Magic Numbers and Bit Layouts

### PMCR_EL0 Initialization Mask
```
E  (bit 0) = 1     Enable PMU
P  (bit 1) = 1     Reset event counters
C  (bit 2) = 1     Reset cycle counter
LC (bit 6) = 1     64-bit cycle counter mode

Combined: (1 << 6) | (1 << 2) | (1 << 1) | (1 << 0) = 0b01000111 = 0x47
```

### PMCNTENSET_EL0 Initialization Mask
```
Cycle counter enable: (1 << 31) = 0x80000000
```

### MIDR_EL1 Field Offsets
```
[31:24] Implementer     (>> 24) & 0xFF
[23:20] Variant         (>> 20) & 0xF
[19:16] Architecture    (>> 16) & 0xF
[15:4]  Part Number     (>> 4)  & 0xFFF
[3:0]   Revision        & 0xF
```

### CTR_EL0 Field Offsets
```
Instr cache: (>> 0) & 0xF  → byte size = (1 << value) * 4
Data cache:  (>> 16) & 0xF → byte size = (1 << value) * 4
```

### ID_AA64PFR0_EL1 Feature Fields
```
FP       [19:16]: (>> 16) & 0xF  (0xF = not present, else present)
AdvSIMD  [23:20]: (>> 20) & 0xF  (0xF = not present, else present)
```

### ID_AA64ISAR0_EL1 Feature Fields
```
AES      [7:4]   : (>> 4)  & 0xF  (0 = not present, else present)
SHA1     [11:8]  : (>> 8)  & 0xF  (0 = not present, else present)
SHA2     [15:12] : (>> 12) & 0xF  (0 = not present, else present)
CRC32    [19:16] : (>> 16) & 0xF  (0 = not present, else present)
RNDR     [63:60] : (>> 60) & 0xF  (0 = not present, else present)
```

### ID_AA64MMFR0_EL1 PARange Decoding
```
PARange [3:0]: & 0xF
0 → 32 bits
1 → 36 bits
2 → 40 bits
3 → 42 bits
4 → 44 bits
5 → 48 bits
6 → 52 bits
other → 0 (invalid)
```

---

## Correctness Constraints and Gotchas

1. **ISB after MSR (subtle):** The C code does not issue an ISB after PMU setup. This is OK because PMU initialization is informational, not context-affecting. However, if future code depends on PMU state being visible immediately, an ISB may be needed. Current: not required.

2. **Cycle counter wrapping:** Counter is 64-bit; wraps at 2^64 cycles. At typical CPU frequencies (e.g., 3 GHz), this is ~200 years before wrap. No practical concern for kernel uptime.

3. **Feature field values:** For FP and AdvSIMD, values 0x0 and 0x1 both mean "present" (0x1 = half-precision variant). The C code treats both as "yes". Port must replicate this (compare `!= 0xF`, not `== 0x0`).

4. **Implementer code 0x41 special case:** Lookup of ARM part names only happens when implementer == 0x41. Other implementers use generic "unknown-part". This conditional must be preserved.

5. **Register immutability:** Once cached at boot, CPU ID registers are never re-read. If porting to multi-CPU systems, each core must call `cpu_init()` independently (and use thread-local or per-CPU storage). Current code assumes single-CPU.

6. **UART dependency:** `cpu_init()` calls `uart_printf()` unconditionally. UART must be initialized first. This is enforced by boot sequence but not by the function itself.

7. **ksnprintf usage:** `cpu_render_info()` uses the kernel's `ksnprintf()` (not standard printf). Port must use the same or compatible formatter.

---

## Testing / Verification Points

1. **Boot-time initialization:** `cpu_init()` called, UART prints implementer + part name + MIDR
2. **Cached values stability:** Multiple calls to `cpu_render_info()` produce identical output (no re-reads of sysregs)
3. **Cycle counter monotonicity:** `cpu_read_cycles()` returns increasing values over time (though not guaranteed to be contiguous if interleaved with other work)
4. **Bit extraction correctness:** Field values match manual inspection of MIDR, CTR, feature registers
5. **String lookup correctness:** Known implementer codes and ARM part numbers produce correct names
6. **Feature decoding:** FP/AdvSIMD presence matches actual feature register values (0xF = absent, else present)

---

## Related Code and References

- **ARMv8-A ARM Manual:** System registers (MIDR_EL1, PMCR_EL0, etc.)
- **uart.h:** UART initialization and printf (prerequisite for cpu_init)
- **strings.h:** ksnprintf formatter (used by cpu_render_info)
- **kernel.c:** cpu_init() call site and boot sequence
