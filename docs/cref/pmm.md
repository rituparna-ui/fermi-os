# PMM (Physical Memory Manager) - Implementation Spec

## Overview

The PMM is a simple bitmap-based page allocator for bare-metal aarch64. It manages a contiguous 8GB physical memory region (`0x40000000` to `0x440000000`) using a bitmap where each bit represents one 4KB page.

**Key Characteristics:**
- Single contiguous memory region (no complex NUMA or fragmented zones)
- Bitmap allocator: O(n) worst-case, O(1) common-case single-page alloc
- Supports both single-page and contiguous multi-page allocation
- Reserved region: kernel image, stack, and bitmap itself (marked used on init)
- Relocates to virtual address after MMU enablement
- No locking (early boot single-threaded, later tasks need spinlock wrapper)
- Used by MMU, heap, and all kernel subsystems

---

## Hardware Constants

```c
#define MEM_START       0x40000000ULL           // Physical base of managed RAM
#define MEM_SIZE        (8ULL * 1024 * 1024 * 1024)  // 8 GB
#define PAGE_SIZE       4096                    // 4KB pages
#define PAGE_SHIFT      12                      // log2(PAGE_SIZE)
#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL  // Higher-half virtual offset
```

**Memory Layout (Physical):**
```
0x40000000 ──── mem_region_start
            └─ Kernel image, stack, BSS
0x40?????? ──── bitmap + reserved pages (PAGE_ALIGN_UP)
0x44000000 ──── mem_region_end (8GB boundary)
```

---

## Bitmap Layout

Each page is represented by 1 bit in a flat bitmap:

```c
#define BITMAP_INDEX(pfn)  ((pfn) / 64)        // Which uint64_t word
#define BITMAP_BIT(pfn)    ((pfn) % 64)        // Which bit in that word

bitmap[i] contains bits for PFN [64*i, 64*i+63]
bit set (1)   = page allocated/reserved
bit clear (0) = page free
```

**Bitmap Sizing:**
```c
total_pages = MEM_SIZE / PAGE_SIZE = 8GB / 4KB = 2,097,152 pages
bitmap_size = (total_pages + 63) / 64 = 32,768 uint64_t entries
bitmap_bytes = 262,144 bytes = 64 pages
```

**Padding:** If `total_pages % 64 != 0`, the unused high bits of the last bitmap word are pre-marked (1) to prevent returning non-existent PFNs.

---

## Static Data

All global/static, initialized by `pmm_init()`:

```c
static uint64_t *bitmap;           // Pointer to bitmap array (relocated on MMU)
static uint64_t bitmap_size;       // Number of uint64_t entries
static uint64_t total_pages;       // MEM_SIZE / PAGE_SIZE
static uint64_t used_pages;        // Tracks allocations (reserved + user)
static uint64_t reserved_pages;    // Kernel + bitmap (immutable post-init)
static uint64_t mem_region_start;  // MEM_START
static uint64_t mem_region_end;    // MEM_START + MEM_SIZE
```

---

## Public API

### `void pmm_init(uintptr_t mem_start, uint64_t mem_size)`

**Called from:** `early_init()` (kernel.c) before MMU enablement.

**Parameters:**
- `mem_start`: Physical address of RAM base (typically `MEM_START = 0x40000000`)
- `mem_size`: Total RAM in bytes (typically `MEM_SIZE = 8GB`)

**Behavior:**
1. Initialize `mem_region_start` and `mem_region_end`
2. Calculate `total_pages = mem_size / PAGE_SIZE`
3. Calculate `bitmap_size = (total_pages + 63) / 64`
4. Place bitmap at `PAGE_ALIGN_UP(__kernel_end)` (first page-aligned address after kernel)
5. Zero the entire bitmap via `memset(bitmap, 0, bitmap_size * 8)`
6. Mark non-existent pages (high bits of last word) as used if `total_pages % 64 != 0`
7. Mark all pages from 0 to `reserved_pages` (kernel + bitmap) as used:
   ```
   reserved_end = PAGE_ALIGN_UP((uintptr_t)bitmap + bitmap_bytes)
   reserved_pages = (reserved_end - mem_region_start) / PAGE_SIZE
   for pfn in [0, reserved_pages): bitmap_set(pfn)
   ```
8. Set `used_pages = reserved_pages`
9. Print initialization info via UART

**Critical Details:**
- Bitmap placement is physical at boot; `PHYS_TO_VIRT()` is NOT applied here
- Reserve calculation uses `PAGE_ALIGN_UP()` to ensure bitmap end is page-aligned
- The `memset()` call assumes identity mapping is still active (pre-MMU)

---

### `void pmm_relocate_upper(void)`

**Called from:** Post-MMU initialization (after virtual address space is live).

**Behavior:**
- Convert bitmap pointer from physical to virtual address:
  ```c
  bitmap = (uint64_t *)PHYS_TO_VIRT((uint64_t)bitmap);
  // bitmap += KERNEL_VA_OFFSET (0xFFFF000000000000)
  ```
- Print relocation message to UART
- All future bitmap access uses the relocated virtual address

**Critical Details:**
- Must be called exactly once after MMU is live
- Any bitmap access before this call uses physical address (identity map active)
- Any bitmap access after this call uses virtual address (must go through page tables)

---

### `uintptr_t pmm_allocate_page(void)`

**Returns:** Physical address of allocated 4KB page, or 0 on failure.

**Algorithm:**
1. Iterate through bitmap words (`bitmap[i]` for `i in [0, bitmap_size)`)
2. Skip words equal to `~0ULL` (all bits set = all pages used)
3. For each non-full word, check bits 0–63 in order:
   - Calculate `pfn = i * 64 + bit`
   - Bounds-check `pfn < total_pages` (catch padding bits)
   - If bit is clear (0), allocate:
     - Set the bit: `bitmap_set(pfn)`
     - Increment `used_pages`
     - Return `phys_addr = mem_region_start + (pfn << PAGE_SHIFT)`
4. If no free page found, print error and return 0

**Complexity:** O(1) amortized for bitmap-based allocator (skip full words).

**Failure Modes:**
- Out of memory → error logged, return 0
- Out-of-range PFN (padding bits) → error logged, return 0

---

### `uintptr_t pmm_allocate_pages(uint64_t count)`

**Returns:** Physical address of first page in contiguous block, or 0 on failure.

**Special Cases:**
- `count == 0` → return 0
- `count == 1` → delegate to `pmm_allocate_page()`

**Algorithm:**
1. Scan PFNs from `reserved_pages` to `total_pages` sequentially
2. Track current run: `run_start` (PFN), `run_length` (count)
3. For each PFN:
   - If free (bit clear):
     - If `run_length == 0`, set `run_start = pfn`
     - Increment `run_length`
     - If `run_length == count`, mark all pages and return:
       ```c
       for i in [0, count): bitmap_set(run_start + i)
       used_pages += count
       return mem_region_start + (run_start << PAGE_SHIFT)
       ```
   - If allocated (bit set):
     - Reset `run_length = 0` (break contiguous run)
4. If no contiguous block of `count` pages found, print error and return 0

**Complexity:** O(total_pages) worst-case (scan entire bitmap).

**Failure Modes:**
- No contiguous block of requested size → error logged, return 0

---

### `void pmm_free_page(uintptr_t phys_addr)`

**Parameter:** Physical address of page to free (must be page-aligned).

**Behavior:**
1. Validate `phys_addr`:
   - Must be in range `[mem_region_start, mem_region_end)`
   - Must be page-aligned (lower 12 bits clear): `phys_addr & 0xFFF == 0`
   - Cannot be in reserved region: `phys_addr >= mem_region_start + (reserved_pages << 12)`
   - Must be allocated (bit set in bitmap)
2. If valid:
   - Calculate `pfn = (phys_addr - mem_region_start) >> PAGE_SHIFT`
   - Clear the bit: `bitmap_clear(pfn)`
   - Decrement `used_pages`
3. If invalid, log error and return silently

**Failure Modes (silent with error message):**
- Address outside managed region
- Non-page-aligned address
- Reserved page
- Unallocated page (double-free)

---

### `void pmm_free_pages(uintptr_t phys_addr, uint64_t count)`

**Parameter:**
- `phys_addr`: Physical address of first page
- `count`: Number of contiguous pages to free

**Behavior:**
- Loop `count` times, calling `pmm_free_page(phys_addr + i * PAGE_SIZE)` for each
- No batching; each page is freed individually

---

### `uint64_t pmm_get_total_pages(void)`

**Returns:** Total number of pages managed (read-only counter).

---

### `uint64_t pmm_get_used_pages(void)`

**Returns:** Number of currently allocated pages (reserved + user allocations).

---

### `uint64_t pmm_get_free_pages(void)`

**Returns:** `total_pages - used_pages`.

---

### `uint64_t pmm_get_reserved_pages(void)`

**Returns:** Number of pages reserved for kernel image, stack, and bitmap (immutable).

---

### `void pmm_print_info(void)`

**Behavior:** Print via UART:
```
[PMM][INFO] Memory region: <start> - <end>
[PMM][INFO] Memory Size: <bytes> | <mbytes> mbytes
[PMM][INFO] Total Pages: <count>
[PMM][INFO] Reserved Pages: <count>
[PMM][INFO] Used Pages: <count>
[PMM][INFO] Free Pages: <count>
```

---

## Addressing Macros

```c
#define PFN_TO_PHYS(pfn)       ((uint64_t)(pfn) << PAGE_SHIFT)
#define PHYS_TO_PFN(addr)      ((uint64_t)(addr) >> PAGE_SHIFT)
#define PAGE_ALIGN_UP(addr)    (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr)  ((addr) & ~(PAGE_SIZE - 1))
#define PHYS_TO_VIRT(pa)       ((pa) + KERNEL_VA_OFFSET)
```

**Critical Note:** PFN operations assume the address/PFN is relative to `mem_region_start`. Raw physical addresses must first be offset:
```c
pfn = PHYS_TO_PFN(phys_addr - mem_region_start);  // Correct
phys_addr = mem_region_start + PFN_TO_PHYS(pfn);  // Correct
```

---

## Boot/Initialization Sequence

**Phase 1: Early Boot (Identity Mapped, Pre-MMU)**
1. `zero_bss()` – Clear BSS section
2. `enable_fp_simd()` – Enable SIMD registers
3. `uart_init()` – Initialize UART
4. `exceptions_init()` – Set up exception handlers
5. **`pmm_init(MEM_START, MEM_SIZE)`** – Initialize PMM (bitmap at physical address)
6. `pmm_print_info()` – Print memory stats
7. `mmu_init()` – Enable MMU and jump to virtual address space

**Phase 2: Post-MMU (Virtual Addressing)**
8. **`pmm_relocate_upper()`** – Relocate bitmap pointer to virtual address
9. `heap_init()` – Initialize kernel heap (uses PMM)
10. Other subsystems initialize and allocate pages via PMM

**Critical Ordering:** PMM must be initialized before MMU, and `pmm_relocate_upper()` must be called after MMU is live but before any further bitmap operations.

---

## Dependencies

**Called by:**
- `kernel.c`: `early_init()`, post-MMU phase
- `mmu.c`: Page table allocation (likely via heap, which uses PMM)
- `heap.c`: Dynamic allocation backing
- All kernel subsystems: Memory allocation

**Calls:**
- `uart.h`: `uart_printf()`, `uart_println()`, `uart_errorln()`
- `strings.h`: `memset()`
- `mmu.h`: `PHYS_TO_VIRT()` macro

**No external synchronization:** Single-threaded at init; assumes no concurrent allocation. Higher-level heap/allocator should add spinlocks if needed.

---

## Rust Port Strategy

### Module Structure
```rust
pub mod pmm {
    // src/mm/pmm/mod.rs or src/mm/pmm.rs
    use core::ptr;
    use core::sync::atomic::{AtomicU64, Ordering};
    
    // Physical memory manager subsystem
}
```

### Types & Statics

```rust
static BITMAP: AtomicPtr<[u64]> = AtomicPtr::new(ptr::null_mut());
static BITMAP_SIZE: AtomicU64 = AtomicU64::new(0);
static TOTAL_PAGES: AtomicU64 = AtomicU64::new(0);
static USED_PAGES: AtomicU64 = AtomicU64::new(0);
static RESERVED_PAGES: AtomicU64 = AtomicU64::new(0);
static MEM_REGION_START: AtomicU64 = AtomicU64::new(0);
static MEM_REGION_END: AtomicU64 = AtomicU64::new(0);
```

**Rationale for Atomics:**
- Init phase is single-threaded; atomics have zero overhead pre-init
- Post-init, counters may be read from other CPUs; `Ordering::SeqCst` ensures visibility
- Bitmap itself still needs spinlock if concurrent access expected (add later)

### Inline Functions (Bitmap Ops)

```rust
#[inline]
fn bitmap_set(pfn: u64) {
    let index = (pfn / 64) as usize;
    let bit = pfn % 64;
    unsafe {
        let bitmap = BITMAP.load(Ordering::Acquire);
        (*bitmap)[index] |= 1u64 << bit;
    }
}

#[inline]
fn bitmap_clear(pfn: u64) {
    let index = (pfn / 64) as usize;
    let bit = pfn % 64;
    unsafe {
        let bitmap = BITMAP.load(Ordering::Acquire);
        (*bitmap)[index] &= !(1u64 << bit);
    }
}

#[inline]
fn bitmap_test(pfn: u64) -> bool {
    let index = (pfn / 64) as usize;
    let bit = pfn % 64;
    unsafe {
        let bitmap = BITMAP.load(Ordering::Acquire);
        ((*bitmap)[index] >> bit) & 1 == 1
    }
}
```

### Constants

```rust
pub const MEM_START: u64 = 0x40000000;
pub const MEM_SIZE: u64 = 8 * 1024 * 1024 * 1024;
pub const PAGE_SIZE: u64 = 4096;
pub const PAGE_SHIFT: u32 = 12;
pub const KERNEL_VA_OFFSET: u64 = 0xFFFF000000000000;

#[inline]
pub const fn pfn_to_phys(pfn: u64) -> u64 {
    pfn << PAGE_SHIFT
}

#[inline]
pub const fn phys_to_pfn(addr: u64) -> u64 {
    addr >> PAGE_SHIFT
}

#[inline]
pub const fn page_align_up(addr: u64) -> u64 {
    (addr + PAGE_SIZE - 1) & !(PAGE_SIZE - 1)
}

#[inline]
pub const fn page_align_down(addr: u64) -> u64 {
    addr & !(PAGE_SIZE - 1)
}

#[inline]
pub const fn phys_to_virt(pa: u64) -> u64 {
    pa + KERNEL_VA_OFFSET
}

#[inline]
pub const fn virt_to_phys(va: u64) -> u64 {
    va - KERNEL_VA_OFFSET
}
```

### Error Handling

Use a custom `enum`:
```rust
pub enum PmmError {
    OutOfMemory,
    NoContiguousBlock,
    OutOfRange,
    NotPageAligned,
    AddressOutOfRegion,
    ReservedPage,
    UnallocatedPage,
}
```

Return `Result<u64, PmmError>` instead of 0 for errors. Wrap C printf calls with `uart::printf()` or similar.

### Locking Strategy (Future)

Current: No locks (single-threaded init phase).

When concurrent access needed:
```rust
use core::sync::atomic::AtomicBool;

static LOCK: Spinlock<()> = Spinlock::new(());

pub fn allocate_page() -> Result<u64, PmmError> {
    let _guard = LOCK.lock();
    // ... allocation logic
}
```

### Assembly Requirements

**No pure-Rust replacement for:**
- Bitmap access is normal memory; no sysreg/barrier needed
- UART output used for debugging (fine as extern C)
- `memset()` is standard C library (use `core::ptr::write_bytes()` in Rust)

**Result:** 100% Rust + inline asm (UART calls), no `.S` files needed.

---

## Gotchas & Correctness

1. **Bitmap Padding:** If `total_pages % 64 != 0`, high bits of last word must be pre-marked to prevent returning non-existent PFNs. Failure → OOB page frame number.

2. **Physical vs. Virtual Bitmap Pointer:**
   - Pre-`pmm_relocate_upper()`: bitmap is at physical address (identity map)
   - Post-`pmm_relocate_upper()`: bitmap is at virtual address (page table lookup)
   - Accessing bitmap with wrong addressing mode → memory fault or stale data

3. **Reserved Region:** Pages 0 to `reserved_pages - 1` are permanently reserved. Freeing them is silently ignored. User must never call `pmm_free_page()` on kernel image.

4. **Contiguous Allocation Scan:** `pmm_allocate_pages()` starts at `reserved_pages`, not 0. This is intentional (reserved pages can't be freed, so no point checking them for fragmentation).

5. **Alignment Invariant:** All returned physical addresses satisfy `phys_addr % PAGE_SIZE == 0`. All input addresses to `pmm_free_page()` must also be aligned.

6. **PFN Offset:** Raw physical address must be offset by `mem_region_start` before converting to PFN:
   ```
   pfn = (phys_addr - mem_region_start) >> PAGE_SHIFT  // Correct
   pfn = phys_addr >> PAGE_SHIFT                       // Wrong!
   ```

7. **Bitmap Word Alignment:** `bitmap_size = ceil(total_pages / 64)`. Allocations are never checked against `bitmap_size * 64`; the padding guard prevents OOB access.

8. **No Reallocation:** Bitmap pointer is set once at init and never moved. `pmm_relocate_upper()` only converts it; the array itself remains in-place.

9. **Counter Overflow:** `used_pages` and `total_pages` are u64; no practical overflow on 8GB system. On larger systems, u64 is still safe (up to 16EB).

---

## Integration Points

**MMU (mmu.h):**
- Calls `pmm_allocate_page()` or `pmm_allocate_pages()` for page table allocation
- Provides `PHYS_TO_VIRT()` macro used by `pmm_relocate_upper()`

**Heap (heap.c):**
- Calls `pmm_allocate_page()` / `pmm_allocate_pages()` for heap backing pages
- Returns freed memory via `pmm_free_page()` / `pmm_free_pages()`

**Kernel (kernel.c):**
- Calls `pmm_init()` during `early_init()`
- Calls `pmm_print_info()` to print memory stats
- Calls `pmm_relocate_upper()` post-MMU

**UART (uart.h):**
- Used by PMM for all debugging output

---

## Hardware & ABI Assumptions

- **Endianness:** Little-endian (aarch64 QEMU default)
- **Atomicity:** u64 loads/stores are atomic on aarch64; no CAS needed for init-phase writes
- **TLB:** PMM relies on identity mapping pre-MMU; post-MMU, must flush TLB if bitmap page is modified (unlikely, should not happen)
- **Barriers:** No explicit barriers needed; `memset()` handles ordering

---

## Test Plan

1. **Boot:** `pmm_init()` completes, bitmap placed after kernel, reserves correct # of pages
2. **Single-Page Alloc:** `pmm_allocate_page()` returns increasing addresses, marks bits
3. **Contiguous Alloc:** `pmm_allocate_pages(N)` finds contiguous free block or returns 0
4. **Free:** `pmm_free_page()` clears bits, decrements counter
5. **Relocation:** `pmm_relocate_upper()` converts bitmap to virtual address, allocation still works
6. **Counters:** `get_free/used/total/reserved_pages()` match expected values
7. **Error Cases:** Freeing reserved/unallocated/misaligned addresses logged as errors
8. **Exhaustion:** Allocating > available free pages returns 0

