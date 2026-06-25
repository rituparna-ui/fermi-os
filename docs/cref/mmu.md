# MMU (Memory Management Unit) - Porting Specification

## Overview

The MMU subsystem implements 3-level page table translation (L0→L1→L2→L3) for aarch64, supporting:
- 4 KiB page granule, 48-bit virtual address space (VAS)
- Dual translation regimes: TTBR0 (user-space, task-specific) and TTBR1 (kernel-space, global)
- 2 MiB block descriptors for initial identity map (L2-level)
- 4 KiB page descriptors for user mappings (L3-level)
- ASID (Address Space ID) support: 16-bit per-task isolation without TLB flushes on context switch
- Higher-half kernel mapping: VA 0xFFFF000000000000+ maps PA 0x0+

**Memory layout:**
- Physical RAM: 0x40000000–0x240000000 (8 GiB) 
- Device I/O, PCI ECAM: 0x0–0x8000000000 (first 512 GB)
- PCI MMIO64: 0x8000000000–0x10000000000 (512 GB–1 TB)
- Virtual address split: bit[47] distinguishes lower half (TTBR0) from upper half (TTBR1)

## Hardware Constants & Register Fields

### PTE (Page Table Entry) Bits

All constants are 64-bit uint64_t unless otherwise noted.

| Constant | Value | Bit Position(s) | Meaning |
|----------|-------|-----------------|---------|
| `PTE_VALID` | `1ULL << 0` | [0] | Descriptor is valid (0/10 = invalid, 01 = block, 11 = table/page) |
| `PTE_TABLE` | `1ULL << 1` | [1] | Entry points to next level (L1/L2/L3 table) or page |
| `PTE_BLOCK` | `0ULL << 1` | [1] | Entry is a 2 MiB block descriptor (L2 only in this kernel) |
| `PTE_ATTRIDX(idx)` | `(idx) << 2` | [4:2] | Memory attribute index (0=Device, 1=Normal) from MAIR_EL1 |
| `PTE_AF` | `1ULL << 10` | [10] | Access Flag: CPU raises fault if AF=0 on first use |
| `PTE_SH_INNER` | `3ULL << 8` | [9:8] | Shareability: inner shareable (0b11) for coherent access |
| `PTE_NG` | `1ULL << 11` | [11] | Non-Global: TLB entry tagged with ASID (TTBR0[63:48]) |
| `PTE_AP_RW` | `0ULL << 6` | [7:6] | Access Permissions: EL1 RW, EL0 no access |
| `PTE_AP_RW_EL0` | `1ULL << 6` | [7:6] | Access Permissions: EL1 RW, EL0 RW |
| `PTE_AP_RO` | `2ULL << 6` | [7:6] | Access Permissions: EL1 RO, EL0 no access |
| `PTE_AP_RO_EL0` | `3ULL << 6` | [7:6] | Access Permissions: EL1 RO, EL0 RO |
| `PTE_UXN` | `1ULL << 54` | [54] | User eXecute Never: prevent EL0 instruction fetch |
| `PTE_PXN` | `1ULL << 53` | [53] | Privileged eXecute Never: prevent EL1 execution |
| `PTE_ADDR_MASK` | `0x0000FFFFFFFFF000ULL` | [47:12] | Physical address mask for 4 KiB granule + 48-bit OA |

### MAIR_EL1 (Memory Attribute Indirection Register)

Defines memory attributes indexed by `PTE_ATTRIDX(idx)`. Each 8-bit field corresponds to one attribute index.

```c
uint64_t mair = (0x00 << 0) |   // AttrIdx 0: Device memory (0x00 = nGnRnE)
                (0xff << 8);    // AttrIdx 1: Normal memory (0xff = Write-Back, Write-Allocate)
```

### TCR_EL1 (Translation Control Register)

Controls page table structure and caching policy for both TTBR0 and TTBR1.

| Field | Value | Bits | Description |
|-------|-------|------|-------------|
| T0SZ | `16` | [5:0] | TTBR0 VA size: 2^(64-16)=2^48, 48-bit VAS |
| IRGN0 | `0b01` | [9:8] | TTBR0 inner cache: Write-Back Write-Allocate |
| ORGN0 | `0b01` | [11:10] | TTBR0 outer cache: Write-Back Write-Allocate |
| SH0 | `0b11` | [13:12] | TTBR0 shareability: inner shareable |
| TG0 | `0b00` | [15:14] | TTBR0 granule: 4 KiB (encoding differs from TG1) |
| T1SZ | `16` | [21:16] | TTBR1 VA size: 2^48 |
| IRGN1 | `0b01` | [25:24] | TTBR1 inner cache: Write-Back Write-Allocate |
| ORGN1 | `0b01` | [27:26] | TTBR1 outer cache: Write-Back Write-Allocate |
| SH1 | `0b11` | [29:28] | TTBR1 shareability: inner shareable |
| TG1 | `0b10` | [31:30] | TTBR1 granule: 4 KiB (bit[31]=RES1, bit[30]=0) |
| IPS | `0b010` | [34:32] | Intermediate PA size: 40-bit (1 TB support) |
| AS | `1` | [36] | ASID size: 1 → 16-bit ASIDs (65536 contexts) |

**Boot value (complete):**
```c
uint64_t tcr = (16ULL << 0) | (0b01ULL << 8) | (0b01ULL << 10) | (0b11ULL << 12) |
               (0b00ULL << 14) | (16ULL << 16) | (0b01ULL << 24) | (0b01ULL << 26) |
               (0b11ULL << 28) | (0b10ULL << 30) | (0b010ULL << 32) | (1ULL << 36);
```

### SCTLR_EL1 (System Control Register) - MMU Enable Bits

| Bit | Name | Function |
|-----|------|----------|
| [0] | M | MMU enable (set to 1 to turn on translation) |
| [2] | C | Data cache enable |
| [12] | I | Instruction cache enable |

### TTBR_EL1 (Translation Table Base Register) Layout

When `TCR_EL1.AS=1` (16-bit ASIDs) and `TCR_EL1.A1=0` (ASID source from TTBR0):

```c
// TTBR packing and unpacking
#define TTBR_ASID_SHIFT 48
#define TTBR_BADDR_MASK 0x0000FFFFFFFFFFFFULL

static inline uint64_t ttbr_pack(uint64_t baddr, uint16_t asid) {
  return (baddr & TTBR_BADDR_MASK) | ((uint64_t)asid << TTBR_ASID_SHIFT);
}
static inline uint64_t ttbr_baddr(uint64_t ttbr) {
  return ttbr & TTBR_BADDR_MASK;
}
static inline uint16_t ttbr_asid(uint64_t ttbr) {
  return (uint16_t)(ttbr >> TTBR_ASID_SHIFT);
}
```

| Field | Bits | Purpose |
|-------|------|---------|
| ASID | [63:48] | Address Space ID (per-task for TTBR0; usually 0 for TTBR1) |
| Base Address | [47:1] | Page-aligned L0 table address (page-aligned so [11:0]=0) |
| CnP | [0] | Common non-shareable pages (leave 0) |

### Address Translation - Virtual to Physical Index Extraction

```c
#define L0_INDEX(va) (((va) >> 39) & 0x1FF)   // VA[47:39] → 9 bits
#define L1_INDEX(va) (((va) >> 30) & 0x1FF)   // VA[38:30] → 9 bits
#define L2_INDEX(va) (((va) >> 21) & 0x1FF)   // VA[29:21] → 9 bits
#define L3_INDEX(va) (((va) >> 12) & 0x1FF)   // VA[20:12] → 9 bits
```

Each level has 512 entries (2^9). Offset within 4 KiB page is VA[11:0].

### Kernel Address Space Constants

```c
#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL  // Upper-half base VA
#define PHYS_TO_VIRT(pa) ((pa) + KERNEL_VA_OFFSET)
#define VIRT_TO_PHYS(va) ((va) - KERNEL_VA_OFFSET)

#define _512GB 0x8000000000ULL
#define _1GB 0x40000000ULL
#define _2MB 0x200000ULL
```

### User Address Space Layout

```c
#define USER_TEXT_BASE 0x00400000ULL        // 4 MiB: user code entry
#define USER_STACK_TOP 0x00800000ULL        // 8 MiB: top of user stack (grows down)
#define USER_STACK_PAGES 4                  // 16 KiB initial stack
```

## Page Table Structure

### Page Table Entry (PTE)

64-bit entry encoding for both table descriptors and page descriptors:
- **Bits [47:12]:** Physical address (4 KiB aligned for pages, 2 MiB aligned for L2 blocks)
- **Bits [11:2]:** Attributes (AP, ATTRIDX, SH, AF, NG)
- **Bits [1:0]:** Descriptor type (00/10=invalid, 01=block, 11=table/page)

### L0 Table (512 entries, 4 KiB)

Covers entire 48-bit VA space via two L1 tables:
- **L0[0]:** VA 0x000000000000–0x7FFFFFFFFF (0–512 GB) → L1 table for RAM + device I/O
- **L0[1]:** VA 0x8000000000–0xFFFFFFFFFF (512 GB–1 TB) → L1 table for PCI MMIO64

Both created during `mmu_init()` for TTBR0 (boot identity map). User tasks get empty L0 via `mmu_create_user_tables()`.

### L1 Table (512 entries, 4 KiB per L1, 512 L1s total)

Each L1 entry points to an L2 table. L1 covers 1 GB per entry.

### L2 Table (512 entries, 4 KiB per L2, 512 L2s total)

- **Boot identity map:** Each L2 entry is a 2 MiB block descriptor
  - RAM (0x40000000–0x240000000): ATTRIDX=1 (Normal memory), AP_RW
  - Device/IO/PCI: ATTRIDX=0 (Device memory), AP_RW
- **User mappings:** L2 entries are table descriptors pointing to L3

### L3 Table (512 entries, 4 KiB per L3)

Only used for user mappings. Each L3 entry is a 4 KiB page descriptor:
- **User .text:** ATTRIDX=1, AP_RO_EL0, UXN, nG=1
- **User stack:** ATTRIDX=1, AP_RW_EL0, UXN (no execute), PXN (kernel no execute), nG=1

## Public API Functions

### uint64_t *mmu_init(void)

**Purpose:** Initialize MMU for boot, set up TTBR0 and TTBR1 dual translation regimes, enable caches and MMU.

**Preconditions:**
- Called from `early_init()` after PMM is initialized
- MMU is off; all pointers are physical addresses
- UART is initialized for logging
- BSS is zeroed, FP/SIMD enabled

**Algorithm:**
1. Configure MAIR_EL1: AttrIdx 0=Device (0x00), AttrIdx 1=Normal (0xff)
2. Build identity page tables for TTBR0 (L0→L1→L2, 2 MB blocks):
   - L0[0] → L1 (0–512 GB): 512 L1 entries → 512×512 L2 tables covering first 512 GB
     - RAM (0x40000000–0x240000000): Normal memory (ATTRIDX=1)
     - Device I/O + PCI ECAM (everything else): Device memory (ATTRIDX=0)
   - L0[1] → L1 (512 GB–1 TB): 512 L1 entries → 512×512 L2 tables for PCI MMIO64 (all device)
   - **Critical:** L2[0] is left invalid (null deref must fault)
3. Build identical identity page tables for TTBR1 (kernel-space mapping)
4. Configure TCR_EL1: 48-bit VA, 4 KiB granule, 16-bit ASIDs, inner shareable, write-back caching
5. Write TTBR0_EL1 and TTBR1_EL1 with DSB ISH barriers
6. Invalidate TLB: `tlbi vmalle1` with DSB ISH and ISB
7. Enable MMU (SCTLR_EL1.M=1), caches (SCTLR_EL1.C=1, I=1)
8. Return L1 table physical address (used by `mmu_run_tests()`)

**Return:** Physical address of L1 table used by TTBR0 identity map (for test reference).

**Assembly barriers:**
- DSB ISH after all table-page stores and before TTBR writes (ARM ARM DDI 0487 §D5.4)
- ISB after MMU enable to ensure effect is visible

### uint64_t *mmu_create_user_tables(void)

**Purpose:** Allocate an empty L0 page table for a new user task. Intermediate tables (L1/L2/L3) are allocated on-demand during `mmu_map_user_range()`.

**Preconditions:**
- Called after `mmu_init()` (MMU on, can use PHYS_TO_VIRT)
- PMM is initialized

**Return:** Physical address of new L0 table (to be stored in task struct, packed into TTBR0 with ASID).

### void mmu_map_user_range(uint64_t *l0, uint64_t va, uint64_t pa, uint64_t pages, uint64_t flags)

**Purpose:** Map a contiguous range of 4 KiB pages into user address space at L3 (page-level), with demand-paging of intermediate L1/L2/L3 tables.

**Parameters:**
- `l0`: Physical address of user L0 table
- `va`: Virtual address (must be 4 KiB aligned)
- `pa`: Physical address (must be 4 KiB aligned)
- `pages`: Number of 4 KiB pages to map
- `flags`: PTE bits to OR in (e.g., `PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN`)

**Preconditions:**
- User L0 table created via `mmu_create_user_tables()`
- VA and PA are 4 KiB aligned
- Called from EL1 with MMU on (derefs via PHYS_TO_VIRT → TTBR1)

**Algorithm:**
1. For each page i in [0, pages):
   - Call `walk_levels(l0, va + i*4096, target_level=3, alloc=1)`
   - Allocate intermediate L1/L2/L3 tables on demand (via `alloc_table()`)
   - Install L3 page descriptor: `pa + i*4096 | PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_NG | flags`
   - PTE_TABLE=1 for L3 descriptors (same bit encoding as for table descriptors)
   - PTE_NG=1 tags entry with ASID from TTBR0[63:48] → TLB isolation without flush on context switch

**Side effects:** May allocate L1/L2/L3 tables. Caller responsible for TLB invalidation (typically `tlbi vae1, X` where X holds ASID + VA).

### void mmu_free_user_tables(uint64_t *l0_phys)

**Purpose:** Recursively free all page table pages (L0, L1, L2, L3) for a user address space. User data pages are freed separately.

**Preconditions:**
- `l0_phys` is a physical address returned by `mmu_create_user_tables()`
- MMU on; dereferences via PHYS_TO_VIRT → TTBR1

**Algorithm:**
1. For each L0 entry:
   - If invalid, skip
   - Extract L1 phys, dereference via PHYS_TO_VIRT
   - For each L1 entry:
     - If invalid, skip
     - If bit[1]=0 (1 GB block, not a table), skip (no L2 to free)
     - Extract L2 phys, dereference via PHYS_TO_VIRT
     - For each L2 entry:
       - If invalid, skip
       - If bit[1]=0 (2 MB block, legacy), skip (no L3 to free)
       - Extract L3 phys, free via `pmm_free_page()`
     - Free L2 via `pmm_free_page()`
   - Free L1 via `pmm_free_page()`
2. Free L0 via `pmm_free_page()`

### void mmu_run_tests(uint64_t *l1_table_phys)

**Purpose:** Self-test suite run immediately after `mmu_init()` to verify MMU correctness before scheduler starts.

**Preconditions:**
- TTBR0 still points to boot identity table (l0_table_lo)
- Called from `early_init()` before any task creation
- `l1_table_phys` = L1 table phys address returned by `mmu_init()`

**Tests:**

1. **test_mmu_enabled():** Verify SCTLR_EL1.M=1
2. **test_identity_mapping():** Allocate page, write value via PA, read back → confirms lower-half identity map works
3. **test_remap_l2():** 
   - Get 2 MiB-aligned chunk (alloc 4 MiB, align to 2 MiB boundary)
   - Rewrite L2 PTE at (L1[1], L2[10]) to point to new phys
   - Flush TLB (`tlbi vmalle1`)
   - Write via VA (1 GB + 20 MiB), read back via PHYS_TO_VIRT(new_phys) → confirms L2 block translation works
   - Restore original PTE, flush TLB, free pages
4. **test_ttbr1_upper_half():**
   - Allocate page, write via lower-half identity map
   - Read via upper-half VA (PHYS_TO_VIRT)
   - Write via upper-half, read via lower-half → confirms TTBR1 upper-half mapping works

**Output:** UART messages: `[MMU TEST] <name>: PASS/FAIL` for each test.

## Static/Private Helper Functions

### static uint64_t *alloc_table(void)

**Purpose:** Allocate a single 4 KiB page for a page table, zero it safely via PHYS_TO_VIRT if MMU is on.

**Returns:** Physical address of zeroed page, or 0 on failure.

**Algorithm:**
1. Call `pmm_allocate_page()` → physical page pointer
2. Read SCTLR_EL1 to check if MMU is on (bit[0])
3. If MMU off: memset directly (PAS phase)
4. If MMU on: dereference via PHYS_TO_VIRT(phys) to route through TTBR1 (safe for all task TTBR0 states)
5. Return physical page pointer

### static uint64_t *build_identity_tables(uint64_t **out_l1)

**Purpose:** Construct 3-level page tables (L0→L1→L2) with 2 MiB block descriptors covering 1 TB physical address space.

**Parameters:**
- `out_l1`: If non-NULL, write L1 table phys address to `*out_l1`

**Algorithm:**
1. Allocate L0 table
2. For each L0[i] (2 entries: 0 for 0–512 GB, 1 for 512 GB–1 TB):
   - Allocate L1 table
   - Set L0[i] = L1_phys | PTE_VALID | PTE_TABLE
   - For each L1[j] (512 entries, each covers 1 GB):
     - Allocate L2 table
     - Set L1[j] = L2_phys | PTE_VALID | PTE_TABLE
     - For each L2[k] (512 entries, each covers 2 MB):
       - Calculate phys_addr = (L0 index)*512 GB + (L1 index)*1 GB + (L2 index)*2 MB
       - If phys_addr == 0, leave invalid (null deref must fault)
       - If phys_addr in RAM range [MEM_START, MEM_START+MEM_SIZE): attr=1 (Normal), else attr=0 (Device)
       - Set L2[k] = phys_addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_ATTRIDX(attr)
3. If `out_l1`, store first L1 table phys address in `*out_l1`
4. Return L0 phys address

### static uint64_t *walk_levels(uint64_t *l0_table, uint64_t va, int target_level, int alloc)

**Purpose:** Walk L0→L1→L2→[L3] page table hierarchy, optionally allocating intermediate tables.

**Parameters:**
- `l0_table`: Physical address of L0 table
- `va`: Virtual address to walk
- `target_level`: 2 (return &L2[index]), or 3 (return &L3[index])
- `alloc`: 1 to allocate missing tables, 0 to fail if missing

**Returns:** Pointer (via PHYS_TO_VIRT → TTBR1) to entry at `target_level`, or 0 on failure.

**Algorithm:**
1. Extract indices: `l0i = L0_INDEX(va)`, `l1i = L1_INDEX(va)`, `l2i = L2_INDEX(va)`, `l3i = L3_INDEX(va)`
2. Dereference L0 via `PHYS_TO_VIRT(l0_table)`
3. Walk L0 → L1:
   - If L0[l0i] invalid and alloc=0: return 0
   - If L0[l0i] invalid and alloc=1: allocate L1, set L0[l0i] = L1_phys | PTE_VALID | PTE_TABLE
   - Extract L1 phys from L0[l0i], dereference via PHYS_TO_VIRT
4. Walk L1 → L2:
   - If L1[l1i] invalid and alloc=0: return 0
   - If L1[l1i] invalid and alloc=1: allocate L2, set L1[l1i] = L2_phys | PTE_VALID | PTE_TABLE
   - Extract L2 phys from L1[l1i], dereference via PHYS_TO_VIRT
5. If target_level=2: return &L2[l2i]
6. Walk L2 → L3:
   - If L2[l2i] invalid and alloc=0: return 0
   - If L2[l2i] invalid and alloc=1: allocate L3, set L2[l2i] = L3_phys | PTE_VALID | PTE_TABLE
   - Extract L3 phys from L2[l2i], dereference via PHYS_TO_VIRT
7. Return &L3[l3i]

**Rationale for PHYS_TO_VIRT:** After MMU enable, dereferencing raw physical pointers through TTBR0 fails when TTBR0 doesn't identity-map RAM (true for user tasks with sparse page tables). All table access routes through TTBR1 kernel mapping to guarantee safety regardless of current TTBR0 state.

## Boot Sequence & Usage Ordering

### Early Boot (Privileged Assembly State - PAS, no MMU)

1. **boot.S `_start`:**
   - Zero stack top: load KERNEL_VA_OFFSET, subtract from symbol to get physical address
   - Set SP to physical stack top
   - Branch to `early_init()` (PC-relative branch works in PAS)

2. **kernel.c `early_init()`:**
   - Zero BSS section
   - Enable FP/SIMD (CPACR_EL1.FPEN=0b11)
   - Initialize UART (allows logging)
   - Call `exceptions_init()` (set up exception handlers)
   - Call `pmm_init(MEM_START, MEM_SIZE)` (initialize free page lists)
   - Call `mmu_init()`:
     - Configure MAIR_EL1, TCR_EL1
     - Build and install TTBR0 (boot identity map L0_table_lo)
     - Build and install TTBR1 (kernel-space L0_table_hi)
     - Enable MMU: SCTLR_EL1.M=1, caches
   - Call `mmu_run_tests()` (verify MMU works; TTBR0 still at boot identity map)
   - Log "MMU Enabled. Jumping to Upper Half"
   - **Return to boot.S**

3. **boot.S (after `early_init` returns, MMU on):**
   - Load upper-half stack top from KERNEL_VA_OFFSET symbol
   - Set SP to upper-half VA
   - Load `kernel_main` address (now available in upper-half VA range)
   - Branch to `kernel_main`

### Runtime (MMU On, Upper-Half VA Execution)

4. **kernel.c `kernel_main()`:**
   - Continue initialization (scheduler, VFS, etc.)
   - When creating first user task:
     - `sched_create_task()` calls `mmu_create_user_tables()` → empty L0
     - `mmu_map_user_range()` populates L1/L2/L3 for .text and stack
     - Pack task's L0 and ASID into TTBR0, store in task struct
     - On context switch: write TTBR0 with new (L0_phys, ASID) pair
     - User TLB entries (mapped with nG=1) are isolated by ASID
     - No TLB flush needed on context switch (ASID change handles isolation)

5. **On task exit:**
   - `mmu_free_user_tables()` walks L0→L1→L2→L3 and frees all table pages
   - User data pages freed separately by scheduler

## Rust Porting Strategy

### Module Structure
```
src/mm/mmu/
  ├── mod.rs           # Public API exports
  ├── init.rs          # mmu_init(), setup logic
  ├── walk.rs          # walk_levels(), table walking
  ├── user.rs          # mmu_create_user_tables(), mmu_map_user_range(), mmu_free_user_tables()
  ├── test.rs          # mmu_run_tests() and helpers
  ├── consts.rs        # All hardware constants (PTE bits, indices, VA offsets)
  └── pte.rs           # PTE struct with safe bitfield accessors
```

### Core Types

```rust
// PTE bitfield with field accessors
#[repr(transparent)]
pub struct Pte(u64);

impl Pte {
    pub const VALID_BIT: u64 = 1;
    pub const TABLE_BIT: u64 = 2;
    pub const AF_BIT: u64 = 1 << 10;
    pub const NG_BIT: u64 = 1 << 11;
    // ... other constants
    
    pub fn from_raw(val: u64) -> Self { Pte(val) }
    pub fn raw(&self) -> u64 { self.0 }
    pub fn is_valid(&self) -> bool { self.0 & Self::VALID_BIT != 0 }
    pub fn is_table(&self) -> bool { self.0 & Self::TABLE_BIT != 0 }
    pub fn next_table_pa(&self) -> u64 { self.0 & PTE_ADDR_MASK }
    // ... other accessors
}

// Page table array wrapper
#[repr(transparent)]
pub struct PageTable {
    entries: [Pte; 512],
}

impl PageTable {
    pub fn new_zeroed() -> &'static mut Self { /* allocate and zero */ }
    pub fn from_pa(pa: u64) -> &'static mut Self { /* convert PA to kernel VA */ }
}

// Descriptor for a user task's address space
pub struct UserAddressSpace {
    l0_phys: u64,  // Physical address of L0 table
    asid: u16,     // Task-specific ASID
}

impl UserAddressSpace {
    pub fn new() -> Result<Self> { /* mmu_create_user_tables() */ }
    pub fn map_range(&mut self, va: u64, pa: u64, pages: u64, flags: u64) -> Result<()> {
        /* mmu_map_user_range() */
    }
    pub fn free(&mut self) { /* mmu_free_user_tables() */ }
}
```

### Static State (Synchronization Strategy)

```rust
// Boot-time identity map L0 tables (pinned, never freed)
static L0_TABLE_LO: OnceCell<u64> = OnceCell::new();  // TTBR0 (user), phys addr
static L0_TABLE_HI: OnceCell<u64> = OnceCell::new();  // TTBR1 (kernel), phys addr

// ASID allocator (16-bit, max 65536 contexts)
static ASID_ALLOCATOR: SpinLock<AsidAllocator> = SpinLock::new(AsidAllocator::new());

struct AsidAllocator {
    next_asid: u16,  // Simple bump allocator; consider bitmap for reuse
}
```

### Locking & Barriers

- **No per-table locks:** Walk functions are called from scheduler (under sched spinlock) or early boot (single-threaded).
- **DSB ISH** after all table-page stores and before TTBR writes (enforced in Rust via unsafe asm blocks).
- **ISB** after SCTLR_EL1 modifications.
- **TLB invalidation:** Caller responsibility (typically scheduler via `tlbi vae1, X`).

### Which Code Stays Assembly

1. **Inline asm for sysreg access:**
   - `mrs`/`msr` for MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1, SCTLR_EL1
   - `dsb ish`, `isb`, `tlbi vmalle1` barriers/flushing

2. **Possibly: .S file for table-walking hot path** (if profiling shows contention, but unlikely given scheduler lock granularity)

3. **Everything else:** Pure Rust with safe abstractions

### Error Handling

```rust
pub enum MmuError {
    AllocFailed,
    InvalidAddress,
    NotMapped,
}

pub type MmuResult<T> = Result<T, MmuError>;
```

### Testing

- Port all 4 test functions from `mmu_run_tests()` into `#[cfg(test)]` modules
- Verify TTBR0/TTBR1 dual translation, identity map, L2 block remapping, upper-half access
- Run during `early_init()` before scheduler (same preconditions as C version)

## Hardware Gotchas & Correctness Notes

1. **L2[0] must be invalid:** Null pointer dereference (VA 0) must fault. Boot identity tables explicitly skip L2[0] by checking `if (phys_addr == 0) continue`.

2. **PHYS_TO_VIRT routing after MMU enable:** Direct physical pointer dereference can fault if TTBR0 doesn't identity-map RAM. All table walks route through TTBR1 kernel mapping via `PHYS_TO_VIRT()` macro.

3. **ASID context isolation:** 
   - User mappings must set PTE_NG=1 (non-global) so TLB entries are tagged with ASID from TTBR0[63:48]
   - Kernel mappings (TTBR1) leave nG=0 (global) so they're visible to all ASIDs
   - On context switch: write TTBR0 with new `(l0_phys, asid)` pair; TLB entries auto-isolate by ASID, no flush needed for user entries

4. **TCR_EL1 barrier ordering:** Write MAIR_EL1 first (must be visible before TCR takes effect), then TCR_EL1, then DSB ISH, then TTBR registers.

5. **TTBR write barriers:** ARM ARM DDI 0487 §D5.4 requires DSB ISH (not ISHST) between table-page stores and TTBR writes so prior page table updates are observable to MMU hardware.

6. **L2 block alignment:** L2 descriptors in identity map are 2 MiB blocks, not 4 KiB pages. L2 indices produce 2 MiB-aligned addresses (by definition: 512 entries × 2 MB = 1 GB per L1).

7. **TG encoding difference:** TG0 (TTBR0 granule) at bits [15:14] and TG1 (TTBR1 granule) at bits [31:30] have different encodings:
   - TG0: 0b00 = 4 KiB
   - TG1: 0b10 = 4 KiB (with RES1 at bit[31])

8. **IPS (Intermediate PA size):** Set to 0b010 (40-bit) to support > 4 GB RAM. With PAGE_SHIFT=12 and 512 entries per level × 3 levels, max VA = 2^48 = 256 TB (but IPS limits OA to 40 bits = 1 TB).

9. **Memory attribute (MAIR_EL1):** 
   - Device memory (0x00 = nGnRnE): used for MMIO registers, doesn't require coherency
   - Normal memory (0xff = Write-Back Write-Allocate): used for RAM, enables L1/L2/L3 data cache fills

10. **User stack growth:** Demand-paged dynamically; initially mapped with `USER_STACK_PAGES=4` (16 KiB). Scheduler grows stack on page fault via `mmu_map_user_range()` and isolated TLB invalidation (`tlbi vae1, X` with per-task ASID).

## References

- ARM Architecture Reference Manual, DDI 0487 (version J): 
  - §D5.4 Summarizing TLB maintenance requirements
  - §D8.3 Translation process and mnemonic encodings
  - §G5.2.5 MAIR_EL1, TCR_EL1, SCTLR_EL1, TTBR_EL1 register descriptions
- Arm TrustZone Memory Architecture white paper (shareability, caching policies)

