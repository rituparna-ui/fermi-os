# Heap Subsystem (kmalloc/kfree) - Porting Specification

## Overview

The Fermi kernel heap allocator is a first-fit buddy-style memory manager with:
- **Metadata format**: Per-block headers with magic sentinels for double-free/UAF detection
- **Allocation strategy**: First-fit search through a linked list of blocks
- **Splitting**: Blocks split when remainder exceeds minimum threshold
- **Coalescing**: Adjacent free blocks merge on kfree (with physical adjacency check)
- **Growth**: Dynamic heap expansion via PMM page allocation; non-contiguous PMM pages handled via region tracking
- **Alignment**: All allocations aligned to 16 bytes
- **Bounds checking**: kfree validates pointer against known heap regions before dereferencing header

## Hardware & ABI Constants

### Memory Configuration
```
PAGE_SIZE = 4096 bytes (0x1000)
PAGE_SHIFT = 12
MEM_START = 0x40000000ULL (physical)
KERNEL_VA_OFFSET = 0xFFFF000000000000ULL
PHYS_TO_VIRT(pa) = (pa) + KERNEL_VA_OFFSET
VIRT_TO_PHYS(va) = (va) - KERNEL_VA_OFFSET
```

### Heap Constants
```
HEAP_INITIAL_PAGES = 256  // 256 * 4KB = 1MB initial heap
HEAP_EXPAND_MIN_PAGES = 64  // Minimum pages allocated per heap_expand call
HEAP_ALIGN = 16  // All allocations aligned to 16 bytes
HEAP_ALIGN_UP(x) = ((x) + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1)
HEAP_MIN_BLOCK_SIZE = 32  // Don't split if remainder < 32 bytes
```

### Block Header Magic Values
```
BLOCK_MAGIC_ALLOC = 0xA110CEDUL  // Magic for allocated block
BLOCK_MAGIC_FREE  = 0xFEEDF1EEUL  // Magic for free block
```

### Data Structure Sizes
```
sizeof(block_header_t) = 24 bytes (size_t[8] + uint32_t[4] + uint32_t[4] + ptr[8])
  - HOWEVER: BLOCK_HEADER_SIZE = HEAP_ALIGN_UP(24) = 32 bytes (aligned to 16)
```

### Region Tracking
```
HEAP_MAX_REGIONS = 16  // Maximum number of disjoint heap regions
```

## Block Header Structure

```c
typedef struct block_header {
  size_t size;               // Usable payload size in bytes (excludes header)
  uint32_t magic;            // BLOCK_MAGIC_ALLOC | BLOCK_MAGIC_FREE
  uint32_t is_free;          // 1 = free, 0 = allocated (mirrors magic)
  struct block_header *next; // Next block in linked list (address order)
} block_header_t;
```

### Layout in Memory
```
+-----+----------+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
| VA  | Offset   | 0                    | 8              |16
+-----+----------+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
| 0x1000 | 0    | size (8 bytes, u64)   | magic  | is_free|
|     | 8-15    | (BLOCK_HEADER_SIZE - 16 padding)      |
+-----+----------+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
| 0x1020 | 16   | *next (8 bytes, ptr)  | (8 bytes pad)   |
+-----+----------+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
| 0x1020 | 32   | [PAYLOAD BEGINS HERE] | size bytes      |
+-----+----------+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

**Key**: Returns to kmalloc caller point at VA+32 (header + BLOCK_HEADER_SIZE).
kfree reconstructs header via `ptr - BLOCK_HEADER_SIZE`.

## Global State

### Static Variables
```c
static block_header_t *heap_head = 0;  // Pointer to first block in linked list

struct heap_region {
  uintptr_t va_start;       // Virtual address of region start
  uint64_t size_bytes;      // Total size of region (includes headers)
};
static struct heap_region regions[HEAP_MAX_REGIONS];
static uint32_t region_count = 0;      // Number of registered regions
```

**Note**: The linked list is maintained in insertion order (not necessarily address order).
Coalescing uses physical adjacency checks to merge blocks safely across non-contiguous PMM pages.

## Public API

### `void heap_init(void)`

**Behavior**:
1. Allocates `HEAP_INITIAL_PAGES` (256) pages from PMM
2. Converts physical address to virtual via `PHYS_TO_VIRT`
3. Clears heap memory with memset
4. Creates single free block spanning entire heap:
   - Block header at VA start
   - `is_free = 1`, `magic = BLOCK_MAGIC_FREE`
   - `size = heap_size - BLOCK_HEADER_SIZE` (usable payload)
   - `next = NULL`
5. Registers region with VA and total size
6. Prints diagnostics via UART

**Precondition**: PMM must be initialized first (heap_init calls pmm_allocate_pages).
**Postcondition**: heap_head points to valid free block; exactly one region registered.

---

### `void *kmalloc(size_t size)`

**Signature**: Returns pointer to allocated memory, or NULL on failure.

**Algorithm** (first-fit with lazy expansion):
1. Reject size==0 (return NULL)
2. Reject sizes that would overflow alignment: `if (size > SIZE_MAX - (HEAP_ALIGN - 1))`
3. Align size: `size = HEAP_ALIGN_UP(size)`
4. For each of 2 attempts:
   a. Walk linked list from heap_head
   b. Find first free block with `block->size >= size`
   c. If found:
      - **Split check**: If `remaining = block->size - size > BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE`:
        * Create new block at `(uint8_t*)block + BLOCK_HEADER_SIZE + size`
        * `new_block->size = remaining - BLOCK_HEADER_SIZE`
        * `new_block->is_free = 1`, `magic = BLOCK_MAGIC_FREE`
        * `new_block->next = block->next`
        * `block->next = new_block`
        * `block->size = size` (exact allocation, no remainder in old block)
      - Mark block as allocated: `is_free = 0`, `magic = BLOCK_MAGIC_ALLOC`
      - Return `(void*)((uint8_t*)block + BLOCK_HEADER_SIZE)` (payload start)
   d. If not found after walking full list, attempt `heap_expand(size)` (once per attempt)
5. On second attempt failure, log error and return NULL

**Alignment**: Caller receives pointer such that `(ptr - BLOCK_HEADER_SIZE)` is header-aligned,
and malloc result is 16-byte aligned (following standard glibc malloc semantics for block headers).

---

### `void kfree(void *ptr)`

**Signature**: Frees allocated block. No return value.

**Algorithm**:
1. If ptr==NULL, return immediately
2. Reconstruct header: `block = (block_header_t*)((uint8_t*)ptr - BLOCK_HEADER_SIZE)`
3. **Bounds check**: Call `addr_in_any_region((uintptr_t)block)` to verify block start falls within
   a registered heap region; if not, log error and return
4. **Magic check**: If `block->magic != BLOCK_MAGIC_ALLOC`:
   - Log diagnostic message (bad magic)
   - Return without freeing (corruption detected)
5. **Double-free check**: If `block->is_free`:
   - Log "double free detected" error
   - Return
6. Mark block as free: `is_free = 1`, `magic = BLOCK_MAGIC_FREE`
7. **Coalescing walk**: Traverse entire linked list from heap_head:
   ```
   for each block B in list:
     while B->is_free && B->next && B->next->is_free:
       if (uintptr_t)B + BLOCK_HEADER_SIZE + B->size == (uintptr_t)B->next:
         // Physically adjacent free blocks; merge
         B->size += BLOCK_HEADER_SIZE + B->next->size
         B->next = B->next->next
       else:
         break  // Gap detected; can't merge
   ```
   This allows coalescing across non-adjacent PMM frames (the check ensures we don't merge blocks
   separated by fragmentation from non-contiguous page allocations).

**Key invariant**: On successful kfree, all mergeable consecutive free blocks are coalesced.

---

### `void heap_print_info(void)`

**Behavior**: Prints diagnostic info via UART:
- Walks linked list, prints each block: address, size, FREE/USED status
- Tallies total_free, total_used, block_count
- Prints summary: block count, used bytes, free bytes, region count

---

### `uint64_t heap_used_bytes(void)`

**Returns**: Sum of `size` field (usable payload only, not headers) for all non-free blocks.

---

### `uint64_t heap_free_bytes(void)`

**Returns**: Sum of `size` field for all free blocks.

---

### `uint64_t heap_total_bytes(void)`

**Returns**: Sum of all registered region `size_bytes` (includes all headers + payload, both allocated and free).

---

### `void heap_run_tests(void)`

**Behavior**: Self-test suite (6 tests):
1. kmalloc returns non-null
2. Write/read allocated memory
3. Multiple allocations return different addresses
4. 1KB allocation works
5. After free, reuse same block
6. Coalesce + realloc larger block

Logs results and calls `heap_print_info()`.

---

## Static Helper Functions

### `static int register_region(uintptr_t va, uint64_t bytes)`

**Behavior**:
- If `region_count >= HEAP_MAX_REGIONS`, log error and return -1
- Append region to `regions[]` array
- Increment `region_count`
- Return 0 on success

**Used by**: heap_init, heap_expand

---

### `static int addr_in_any_region(uintptr_t addr)`

**Behavior**:
- For each region in `regions[]`:
  - If `addr >= va_start && addr < va_start + size_bytes`, return 1
- Return 0 (address not in any known region)

**Used by**: kfree (bounds check before dereferencing header)

---

### `static int heap_expand(size_t need_bytes)`

**Behavior**:
1. Calculate pages needed:
   - `bytes_required = need_bytes + BLOCK_HEADER_SIZE`
   - `pages = (bytes_required + PAGE_SIZE - 1) / PAGE_SIZE` (round up)
   - If `pages < HEAP_EXPAND_MIN_PAGES`, use `HEAP_EXPAND_MIN_PAGES`
2. Allocate pages from PMM: `phys = pmm_allocate_pages(pages)`
   - If pmm_allocate_pages fails, log error and return -1
3. Convert to virtual: `va = PHYS_TO_VIRT(phys)`
4. Clear memory: `memset((void*)va, 0, pages * PAGE_SIZE)`
5. Create free block at VA:
   - `new_block->size = pages * PAGE_SIZE - BLOCK_HEADER_SIZE`
   - `new_block->is_free = 1`, `magic = BLOCK_MAGIC_FREE`
   - `new_block->next = NULL`
6. **Append to list**: Find last block (tail), set `tail->next = new_block`
7. **Register region**: Call `register_region(va, pages * PAGE_SIZE)`
   - On failure (region table full):
     * Unlink new block: `tail->next = NULL`
     * Free pages: `pmm_free_pages(phys, pages)`
     * Return -1
8. Log expansion details and return 0

**Used by**: kmalloc (on first attempt failure)

---

## Rust Porting Strategy

### Module Structure
```
src/mm/heap/
├── lib.rs           (module exports, public API)
├── allocator.rs     (GlobalAlloc impl, kmalloc/kfree logic)
├── block.rs         (block_header_t struct, splitting/coalescing)
└── region.rs        (region tracking, registration)
```

### Key Types

#### `BlockHeader`
```rust
#[repr(C, align(16))]
pub struct BlockHeader {
    pub size: u64,                      // usable payload size
    pub magic: u32,                     // BLOCK_MAGIC_ALLOC | BLOCK_MAGIC_FREE
    pub is_free: u32,                   // 1 or 0
    pub next: Option<NonNull<BlockHeader>>,
    // Padding to 32 bytes (BLOCK_HEADER_SIZE)
}

const BLOCK_HEADER_SIZE: usize = 32;
const BLOCK_MAGIC_ALLOC: u32 = 0xA110CEDu32;
const BLOCK_MAGIC_FREE: u32 = 0xFEEDF1EEu32;
```

#### `HeapRegion`
```rust
#[repr(C)]
struct HeapRegion {
    va_start: u64,
    size_bytes: u64,
}

static REGIONS: [HeapRegion; HEAP_MAX_REGIONS] = [/* ... */];
static REGION_COUNT: AtomicU32 = AtomicU32::new(0);
```

#### Global State
```rust
static HEAP_HEAD: AtomicPtr<BlockHeader> = AtomicPtr::new(null_mut());

// Static once-initialized guard for heap_init idempotency
static HEAP_INIT_DONE: AtomicBool = AtomicBool::new(false);
```

### Locking Strategy

**Key observation**: The C heap is entirely single-threaded (no locks; synchronization via PMM).
For Rust port:
- **Option A (if kernel is single-threaded at init time)**: No locks; use UnsafeCell + single-threaded invariant documented
- **Option B (future-proof)**: Protect heap_head + region tracking with a spinlock
  ```rust
  lazy_static! {
      static ref HEAP_LOCK: SpinLock<HeapState> = SpinLock::new(HeapState::new());
  }
  ```

For porting purposes, **assume single-threaded execution** (matching C kernel design).
Document that future multi-threaded kernel will need spinlock wrapper.

### GlobalAlloc Implementation
```rust
pub struct KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = layout.size();
        kmalloc(size) as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        kfree(ptr as *mut _)
    }
}

#[global_allocator]
static GLOBAL: KernelAllocator = KernelAllocator;
```

### C FFI Bindings Needed
```rust
extern "C" {
    fn pmm_allocate_pages(count: u64) -> usize;  // returns phys addr
    fn pmm_free_pages(phys_addr: usize, count: u64);
    fn uart_println(s: *const u8);
    fn uart_printf(fmt: *const u8, ...);
}

fn phys_to_virt(pa: u64) -> u64 {
    pa + 0xFFFF000000000000u64
}
fn virt_to_phys(va: u64) -> u64 {
    va - 0xFFFF000000000000u64
}
```

### Assembly Requirements
**None** — pure Rust for this subsystem. All address translation and logic can be expressed in Rust.
Memory barriers may be needed if multi-threaded locking is added later.

---

## Boot / Initialization Ordering

1. **PMM must initialize first** (pmm_init called before heap_init)
   - Sets up physical page bitmap
2. **heap_init** is called early in kernel_main (after cpu_init, before GIC/PCI)
   - Allocates initial heap region
   - Registers with region tracker
3. **kmalloc/kfree available** immediately after heap_init returns
4. **heap_run_tests** can be called for validation (optional)

**Call chain from kernel.c**:
```
kernel_main():
  cpu_init()
  heap_init()     <-- Heap becomes operational
  gic_init()
  pci_enumerate_bus()
  ...
```

---

## Gotchas & Correctness Issues

1. **SIZE_MAX overflow check**: `kmalloc` must reject sizes where `size > SIZE_MAX - (HEAP_ALIGN - 1)`
   - Otherwise HEAP_ALIGN_UP wraps around and allocates tiny blocks
   - This catches real-world callers passing gigantic sizes

2. **Physical adjacency check in coalescing**: 
   - Two free blocks may be address-consecutive but NOT physically adjacent if PMM fragmented
   - Check: `end_of_current == (uintptr_t)next` before merging
   - Without this, coalescing corrupts intervening metadata

3. **Double-free detection via magic**:
   - kfree checks both `is_free` flag AND magic before accepting a free
   - If magic != BLOCK_MAGIC_ALLOC, refuse operation (return without freeing)
   - This catches: double-free (magic rewritten to FREE), corruption (magic clobbered), wild pointers

4. **Bounds check before header dereference**:
   - kfree must validate pointer falls within a registered region BEFORE reading block->magic
   - Otherwise wild pointers could dereference arbitrary kernel memory

5. **Region table overflow**:
   - If region_count reaches HEAP_MAX_REGIONS (16) and heap_expand is called:
     * New pages are allocated by PMM but NOT registered
     * New block is unlinked (tail->next = NULL)
     * Pages are freed back (pmm_free_pages)
     * kmalloc returns NULL
   - This prevents silent corruption from stale region pointers

6. **Split threshold**:
   - Only split if remainder > BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE (= 64 bytes)
   - Otherwise, allocate the whole block (waste is acceptable to avoid header fragmentation)

7. **Alignment enforcement**:
   - All size parameters go through HEAP_ALIGN_UP(size)
   - Block headers themselves are BLOCK_HEADER_SIZE (32 bytes, 16-aligned)
   - User payload always starts at header + 32, which is 16-aligned

8. **memset on allocation**:
   - heap_init and heap_expand both memset(0) their regions
   - Ensures no stale data in payload or headers
   - Important for security (data leftover from previous allocations visible to new allocator)

---

## Hardware Details & Magic Numbers

### Constants (Exact)
| Constant | Value | Usage |
|----------|-------|-------|
| PAGE_SIZE | 4096 (0x1000) | PMM unit |
| PAGE_SHIFT | 12 | PMM unit |
| KERNEL_VA_OFFSET | 0xFFFF000000000000 | Higher-half kernel VA |
| HEAP_INITIAL_PAGES | 256 | Initial 1MB heap |
| HEAP_EXPAND_MIN_PAGES | 64 | Minimum pages per expand call |
| HEAP_ALIGN | 16 | Allocation alignment |
| HEAP_MIN_BLOCK_SIZE | 32 | Minimum split remainder |
| BLOCK_HEADER_SIZE | 32 (aligned 16) | Per-block metadata |
| BLOCK_MAGIC_ALLOC | 0xA110CEDu32 | Allocated sentinel |
| BLOCK_MAGIC_FREE | 0xFEEDF1EEu32 | Free sentinel |
| HEAP_MAX_REGIONS | 16 | Max disjoint regions |
| SIZE_MAX | 0xFFFFFFFFFFFFFFFFu64 | Max malloc size (u64) |

### Struct Layout (Exact)
```
BlockHeader (24 bytes actual, 32 bytes with padding):
  Offset 0-7:   size (u64)
  Offset 8-11:  magic (u32)
  Offset 12-15: is_free (u32)
  Offset 16-23: *next (u64 pointer)
  Offset 24-31: [padding to 32-byte alignment]

User payload starts at header + 32
```

### MMU Macros Used
```
PHYS_TO_VIRT(pa) = (pa) + 0xFFFF000000000000
VIRT_TO_PHYS(va) = (va) - 0xFFFF000000000000
```

---

## Integration Points

### Dependencies
- **PMM (Physical Memory Manager)**: pmm_allocate_pages, pmm_free_pages
- **UART (debugging/logging)**: uart_println, uart_printf, uart_errorln
- **Strings**: memset (for clearing memory)
- **MMU**: PHYS_TO_VIRT constant (for VA translation)

### Dependents (who calls heap)
- **Kernel main (kernel.c)**: heap_init called early
- **Future subsystems**: kmalloc/kfree used by all dynamic allocation (driver buffers, etc.)

### Exported Public Symbols
```
kmalloc(size_t) -> void*
kfree(void*)
heap_init()
heap_print_info()
heap_used_bytes() -> u64
heap_free_bytes() -> u64
heap_total_bytes() -> u64
heap_run_tests()
```

