//! Kernel heap — a first-fit free-list allocator backed by the PMM.
//!
//! Each block is `[BlockHeader | payload]`. Blocks are kept in a single
//! address-ordered (insertion-ordered) singly-linked list. `kmalloc` first-fits
//! and splits; `kfree` validates a magic sentinel (catching double-free,
//! use-after-free, wild pointers), then coalesces *physically adjacent* free
//! neighbours (the PMM may hand back non-contiguous frames across expands, so
//! adjacency is checked before merging).
//!
//! A `GlobalAlloc` adapter (`KernelAllocator`) is installed as `#[global_alloc]`
//! so `alloc::` (Box, Vec, String, BTreeMap…) is available kernel-wide.

use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::{phys_to_virt, PAGE_SIZE};
use crate::mm::pmm;
use core::ptr;

/// 256 * 4 KiB = 1 MiB initial heap.
const HEAP_INITIAL_PAGES: u64 = 256;
/// Don't split a block if the remainder would be smaller than this.
const HEAP_MIN_BLOCK_SIZE: usize = 32;
/// All allocations are 16-byte aligned.
const HEAP_ALIGN: usize = 16;
/// Grow at least this many pages per expand, to amortize PMM + bookkeeping.
const HEAP_EXPAND_MIN_PAGES: u64 = 64;

const BLOCK_MAGIC_ALLOC: u32 = 0x0A11_0CED;
const BLOCK_MAGIC_FREE: u32 = 0xFEED_F1EE;

const HEAP_MAX_REGIONS: usize = 16;

#[inline]
const fn align_up(x: usize) -> usize {
    (x + HEAP_ALIGN - 1) & !(HEAP_ALIGN - 1)
}

/// Block metadata, immediately preceding the payload.
#[repr(C)]
struct BlockHeader {
    size: usize,            // usable payload size (excludes header)
    magic: u32,             // BLOCK_MAGIC_ALLOC | BLOCK_MAGIC_FREE
    is_free: u32,           // 1 = free, 0 = allocated (mirror of magic)
    next: *mut BlockHeader, // next block in the list (insertion order)
}

/// Header size, rounded up to the allocation alignment.
const BLOCK_HEADER_SIZE: usize = align_up(core::mem::size_of::<BlockHeader>());

#[derive(Clone, Copy)]
struct Region {
    va_start: usize,
    size_bytes: u64,
}

struct Heap {
    head: *mut BlockHeader,
    regions: [Region; HEAP_MAX_REGIONS],
    region_count: usize,
}

// SAFETY: the raw block pointers are only walked while holding the heap lock.
unsafe impl Send for Heap {}

impl Heap {
    const fn new() -> Self {
        Self {
            head: ptr::null_mut(),
            regions: [Region {
                va_start: 0,
                size_bytes: 0,
            }; HEAP_MAX_REGIONS],
            region_count: 0,
        }
    }

    fn register_region(&mut self, va: usize, bytes: u64) -> bool {
        if self.region_count >= HEAP_MAX_REGIONS {
            Uart.errorln("[HEAP] region table full");
            return false;
        }
        self.regions[self.region_count] = Region {
            va_start: va,
            size_bytes: bytes,
        };
        self.region_count += 1;
        true
    }

    fn addr_in_any_region(&self, addr: usize) -> bool {
        self.regions[..self.region_count]
            .iter()
            .any(|r| addr >= r.va_start && (addr as u64) < r.va_start as u64 + r.size_bytes)
    }

    /// Grow the heap by allocating more PMM pages, appended as one free block.
    /// Returns true on success.
    fn expand(&mut self, need_bytes: usize) -> bool {
        let bytes_required = need_bytes as u64 + BLOCK_HEADER_SIZE as u64;
        let mut pages = (bytes_required + PAGE_SIZE - 1) / PAGE_SIZE;
        if pages < HEAP_EXPAND_MIN_PAGES {
            pages = HEAP_EXPAND_MIN_PAGES;
        }

        let phys = pmm::allocate_pages(pages);
        if phys == 0 {
            Uart.errorln("[HEAP] expand: allocate_pages failed");
            return false;
        }

        let va = phys_to_virt(phys) as usize;
        let bytes = pages * PAGE_SIZE;
        unsafe {
            ptr::write_bytes(va as *mut u8, 0, bytes as usize);
            let new_block = va as *mut BlockHeader;
            (*new_block).size = bytes as usize - BLOCK_HEADER_SIZE;
            (*new_block).is_free = 1;
            (*new_block).magic = BLOCK_MAGIC_FREE;
            (*new_block).next = ptr::null_mut();

            // Append at the list tail.
            let mut tail = self.head;
            while !(*tail).next.is_null() {
                tail = (*tail).next;
            }
            (*tail).next = new_block;

            if !self.register_region(va, bytes) {
                // Region table overflow — unlink and return the pages.
                (*tail).next = ptr::null_mut();
                pmm::free_pages(phys, pages);
                return false;
            }
        }

        kprintln!(
            "[HEAP] Expanded by {} KiB ({} pages) at VA {:#x}",
            bytes / 1024,
            pages,
            va
        );
        true
    }

    /// First-fit allocate `size` bytes (already alignment-rounded). Returns a
    /// payload pointer or null.
    fn alloc(&mut self, size: usize) -> *mut u8 {
        // Two passes: search, expand on miss, search once more.
        for _ in 0..2 {
            let mut current = self.head;
            while !current.is_null() {
                unsafe {
                    if (*current).is_free == 1 && (*current).size >= size {
                        let remaining = (*current).size - size;
                        if remaining > BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE {
                            // Split: [hdr|alloc][new_hdr|remaining free].
                            let new_block = (current as *mut u8)
                                .add(BLOCK_HEADER_SIZE + size)
                                as *mut BlockHeader;
                            (*new_block).size = remaining - BLOCK_HEADER_SIZE;
                            (*new_block).is_free = 1;
                            (*new_block).magic = BLOCK_MAGIC_FREE;
                            (*new_block).next = (*current).next;

                            (*current).size = size;
                            (*current).next = new_block;
                        }

                        (*current).is_free = 0;
                        (*current).magic = BLOCK_MAGIC_ALLOC;
                        return (current as *mut u8).add(BLOCK_HEADER_SIZE);
                    }
                    current = (*current).next;
                }
            }

            if !self.expand(size) {
                break;
            }
        }

        Uart.errorln("[HEAP] kmalloc: out of memory!");
        ptr::null_mut()
    }

    /// Free a payload pointer, with validation + physical-adjacency coalescing.
    fn free(&mut self, payload: *mut u8) {
        if payload.is_null() {
            return;
        }
        let block = unsafe { payload.sub(BLOCK_HEADER_SIZE) as *mut BlockHeader };
        let block_addr = block as usize;

        if !self.addr_in_any_region(block_addr) {
            Uart.errorln("[HEAP] kfree: pointer outside heap regions!");
            return;
        }

        unsafe {
            if (*block).magic != BLOCK_MAGIC_ALLOC {
                // Wild pointer, already-freed block, or clobbered header.
                kprintln!(
                    "[HEAP] kfree: bad magic {:#x} at {:#x} — refusing",
                    (*block).magic,
                    block_addr
                );
                return;
            }
            if (*block).is_free == 1 {
                Uart.errorln("[HEAP] kfree: double free detected!");
                return;
            }

            (*block).is_free = 1;
            (*block).magic = BLOCK_MAGIC_FREE;

            // Coalesce physically-adjacent free neighbours.
            let mut current = self.head;
            while !current.is_null() {
                while (*current).is_free == 1
                    && !(*current).next.is_null()
                    && (*(*current).next).is_free == 1
                {
                    let end_of_current =
                        current as usize + BLOCK_HEADER_SIZE + (*current).size;
                    if end_of_current != (*current).next as usize {
                        break; // gap — can't safely merge
                    }
                    (*current).size += BLOCK_HEADER_SIZE + (*(*current).next).size;
                    (*current).next = (*(*current).next).next;
                }
                current = (*current).next;
            }
        }
    }
}

static HEAP: SpinLock<Heap> = SpinLock::new(Heap::new());

/// Initialize the heap with `HEAP_INITIAL_PAGES` of PMM-backed memory.
pub fn init() {
    kprintln!("[HEAP] Initializing");

    let phys = pmm::allocate_pages(HEAP_INITIAL_PAGES);
    if phys == 0 {
        Uart.errorln("[HEAP] Failed to allocate pages for heap");
        return;
    }
    let va = phys_to_virt(phys) as usize;
    let heap_size = HEAP_INITIAL_PAGES * PAGE_SIZE;

    let mut heap = HEAP.lock();
    unsafe {
        ptr::write_bytes(va as *mut u8, 0, heap_size as usize);
        let head = va as *mut BlockHeader;
        (*head).size = heap_size as usize - BLOCK_HEADER_SIZE;
        (*head).is_free = 1;
        (*head).magic = BLOCK_MAGIC_FREE;
        (*head).next = ptr::null_mut();
        heap.head = head;
        let usable = (*head).size;
        heap.register_region(va, heap_size);
        drop(heap);

        kprintln!("[HEAP] Heap VA: {:#x} - {:#x}", va, va + heap_size as usize);
        kprintln!(
            "[HEAP] Usable: {} KiB ({} bytes) | Header: {} bytes",
            usable / 1024,
            usable,
            BLOCK_HEADER_SIZE
        );
    }
    kprintln!("[HEAP] Initialized!");
}

/// Allocate `size` bytes (16-byte aligned). Returns null on failure.
pub fn kmalloc(size: usize) -> *mut u8 {
    if size == 0 {
        return ptr::null_mut();
    }
    // Reject sizes that would overflow the alignment rounding.
    if size > usize::MAX - (HEAP_ALIGN - 1) {
        Uart.errorln("[HEAP] kmalloc: size overflow");
        return ptr::null_mut();
    }
    let size = align_up(size);
    HEAP.lock().alloc(size)
}

/// Free a pointer previously returned by `kmalloc`.
pub fn kfree(ptr: *mut u8) {
    HEAP.lock().free(ptr);
}

/// Total bytes currently allocated (payloads only, excludes headers).
pub fn used_bytes() -> u64 {
    let heap = HEAP.lock();
    let mut total = 0u64;
    let mut c = heap.head;
    while !c.is_null() {
        unsafe {
            if (*c).is_free == 0 {
                total += (*c).size as u64;
            }
            c = (*c).next;
        }
    }
    total
}

/// Total bytes currently free (payloads only, excludes headers).
pub fn free_bytes() -> u64 {
    let heap = HEAP.lock();
    let mut total = 0u64;
    let mut c = heap.head;
    while !c.is_null() {
        unsafe {
            if (*c).is_free == 1 {
                total += (*c).size as u64;
            }
            c = (*c).next;
        }
    }
    total
}

/// Aggregate size of all PMM-backed heap regions.
pub fn total_bytes() -> u64 {
    let heap = HEAP.lock();
    heap.regions[..heap.region_count]
        .iter()
        .map(|r| r.size_bytes)
        .sum()
}

/// Print the heap block list and summary.
pub fn print_info() {
    kprintln!("[HEAP][INFO] Heap block list:");
    let heap = HEAP.lock();
    let (mut total_free, mut total_used, mut block_count) = (0u64, 0u64, 0u64);
    let mut c = heap.head;
    while !c.is_null() {
        unsafe {
            kprintln!(
                "  [{}] addr={:#x} size={} {}",
                block_count,
                c as usize,
                (*c).size,
                if (*c).is_free == 1 { "FREE" } else { "USED" }
            );
            if (*c).is_free == 1 {
                total_free += (*c).size as u64;
            } else {
                total_used += (*c).size as u64;
            }
            block_count += 1;
            c = (*c).next;
        }
    }
    kprintln!(
        "[HEAP][INFO] Blocks: {} | Used: {} bytes | Free: {} bytes | Regions: {}",
        block_count,
        total_used,
        total_free,
        heap.region_count
    );
}

fn test_result(name: &str, pass: bool) {
    kprintln!("[HEAP TEST] {}: {}", name, if pass { "PASS" } else { "FAIL" });
}

/// Run the heap self-tests (mirrors the C `heap_run_tests`).
pub fn run_tests() {
    kprintln!("[HEAP TEST] Running heap tests...");

    let a = kmalloc(8) as *mut u64;
    test_result("kmalloc returns non-null", !a.is_null());
    if !a.is_null() {
        unsafe {
            a.write_volatile(0xDEADBEEF);
            test_result("write/read", a.read_volatile() == 0xDEADBEEF);
        }
    }

    let b = kmalloc(8) as *mut u64;
    test_result("different addresses", a != b);

    let buf = kmalloc(1024);
    test_result("1KB alloc", !buf.is_null());
    if !buf.is_null() {
        unsafe {
            ptr::write_bytes(buf, b'A', 1024);
            test_result(
                "1KB write/read",
                buf.read() == b'A' && buf.add(1023).read() == b'A',
            );
        }
    }

    kfree(a as *mut u8);
    kfree(b as *mut u8);
    let c = kmalloc(8) as *mut u64;
    test_result("free/reuse", c == a || c == b);

    kfree(buf);
    kfree(c as *mut u8);
    let buf2 = kmalloc(2048);
    test_result("coalesce + realloc", !buf2.is_null());

    kfree(buf2);

    print_info();
    kprintln!("[HEAP TEST] Done!");
}

// --- GlobalAlloc adapter -----------------------------------------------------

use core::alloc::{GlobalAlloc, Layout};

/// `GlobalAlloc` over the kernel heap. The heap guarantees 16-byte alignment;
/// larger alignment requests are satisfied by over-allocating and storing the
/// original payload pointer just before the aligned pointer.
pub struct KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        if align <= HEAP_ALIGN {
            return kmalloc(layout.size());
        }
        // Over-allocate: room for alignment slack + a back-pointer slot.
        let total = layout.size() + align + core::mem::size_of::<usize>();
        let raw = kmalloc(total);
        if raw.is_null() {
            return raw;
        }
        let base = raw as usize + core::mem::size_of::<usize>();
        let aligned = (base + align - 1) & !(align - 1);
        // Stash the original pointer immediately before the aligned address.
        // SAFETY: the slot lies within the over-allocation we just made.
        unsafe {
            ((aligned - core::mem::size_of::<usize>()) as *mut usize).write(raw as usize);
        }
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= HEAP_ALIGN {
            kfree(ptr);
            return;
        }
        // Recover the original payload pointer stored just before `ptr`.
        // SAFETY: `ptr` came from `alloc` above, which stored the raw pointer in
        // the preceding usize slot.
        let raw = unsafe { ((ptr as usize - core::mem::size_of::<usize>()) as *const usize).read() };
        kfree(raw as *mut u8);
    }
}

#[global_allocator]
static ALLOCATOR: KernelAllocator = KernelAllocator;
