//! Kernel heap — first-fit allocator with split/coalesce, grow-on-demand, and
//! magic sentinels for use-after-free / double-free detection.
//!
//! Direct port of the original `src/mm/heap/heap.c`. Runs after the MMU is on,
//! so it logs via `kprintln!` and guards its metadata with a `SpinLock`. Heap
//! memory is taken from the PMM and accessed through the TTBR1 upper half
//! (`phys_to_virt`). Also provides the `#[global_allocator]` so `alloc` types
//! (Vec/Box/String) work across the kernel.

use crate::kprintln;
use crate::mm::mmu::phys_to_virt;
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::sync::SpinLock;
use crate::uart;
use core::alloc::{GlobalAlloc, Layout};

const HEAP_INITIAL_PAGES: u64 = 256;
const HEAP_MIN_BLOCK_SIZE: usize = 32;
const HEAP_ALIGN: usize = 16;
const BLOCK_MAGIC_ALLOC: u32 = 0x0A11_0CED;
const BLOCK_MAGIC_FREE: u32 = 0xFEED_F1EE;
const HEAP_MAX_REGIONS: usize = 16;
const HEAP_EXPAND_MIN_PAGES: u64 = 64;

/// align_up to 16 bytes.
#[inline(always)]
fn align_up(x: usize) -> usize {
    (x + HEAP_ALIGN - 1) & !(HEAP_ALIGN - 1)
}

#[repr(C)]
struct BlockHeader {
    size: usize,  // usable payload size (excludes header)
    magic: u32,   // BLOCK_MAGIC_ALLOC | BLOCK_MAGIC_FREE
    is_free: u32, // 1 = free, 0 = allocated
    next: usize,  // *mut BlockHeader as usize, 0 = null
}

// sizeof(BlockHeader) = 24, aligned up to 16 = 32.
const BLOCK_HEADER_SIZE: usize = 32;

#[inline(always)]
unsafe fn hdr<'a>(addr: usize) -> &'a mut BlockHeader {
    &mut *(addr as *mut BlockHeader)
}

#[derive(Clone, Copy)]
struct Region {
    va_start: usize,
    size_bytes: u64,
}

struct Heap {
    head: usize, // *mut BlockHeader, 0 = null
    regions: [Region; HEAP_MAX_REGIONS],
    region_count: u32,
}

static HEAP: SpinLock<Heap> = SpinLock::new(Heap {
    head: 0,
    regions: [Region {
        va_start: 0,
        size_bytes: 0,
    }; HEAP_MAX_REGIONS],
    region_count: 0,
});

impl Heap {
    fn register_region(&mut self, va: usize, bytes: u64) -> bool {
        if self.region_count as usize >= HEAP_MAX_REGIONS {
            uart::errorln("[HEAP] region table full");
            return false;
        }
        self.regions[self.region_count as usize] = Region {
            va_start: va,
            size_bytes: bytes,
        };
        self.region_count += 1;
        true
    }

    fn addr_in_any_region(&self, addr: usize) -> bool {
        for i in 0..self.region_count as usize {
            let r = self.regions[i];
            if addr >= r.va_start && (addr as u64) < r.va_start as u64 + r.size_bytes {
                return true;
            }
        }
        false
    }

    /// Grow the heap by at least HEAP_EXPAND_MIN_PAGES. Returns success.
    fn expand(&mut self, need_bytes: usize) -> bool {
        let bytes_required = need_bytes as u64 + BLOCK_HEADER_SIZE as u64;
        let mut pages = (bytes_required + PAGE_SIZE - 1) / PAGE_SIZE;
        if pages < HEAP_EXPAND_MIN_PAGES {
            pages = HEAP_EXPAND_MIN_PAGES;
        }
        let phys = pmm::allocate_pages(pages);
        if phys == 0 {
            uart::errorln("[HEAP] expand: pmm_allocate_pages failed");
            return false;
        }
        let va = phys_to_virt(phys) as usize;
        let bytes = pages * PAGE_SIZE;
        unsafe {
            core::ptr::write_bytes(va as *mut u8, 0, bytes as usize);
            let nb = hdr(va);
            nb.size = bytes as usize - BLOCK_HEADER_SIZE;
            nb.is_free = 1;
            nb.magic = BLOCK_MAGIC_FREE;
            nb.next = 0;

            // Append at end of list.
            let mut tail = self.head;
            while hdr(tail).next != 0 {
                tail = hdr(tail).next;
            }
            hdr(tail).next = va;

            if !self.register_region(va, bytes) {
                hdr(tail).next = 0;
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
}

pub fn init() {
    uart::println("[HEAP] Initializing");
    let pages = HEAP_INITIAL_PAGES;
    let phys = pmm::allocate_pages(pages);
    if phys == 0 {
        uart::errorln("[HEAP] Failed to allocate pages for heap");
        return;
    }
    let va = phys_to_virt(phys) as usize;
    let heap_size = pages * PAGE_SIZE;
    let mut h = HEAP.lock();
    unsafe {
        core::ptr::write_bytes(va as *mut u8, 0, heap_size as usize);
        let head = hdr(va);
        head.size = heap_size as usize - BLOCK_HEADER_SIZE;
        head.is_free = 1;
        head.magic = BLOCK_MAGIC_FREE;
        head.next = 0;
    }
    h.head = va;
    h.register_region(va, heap_size);
    kprintln!("[HEAP] Heap VA: {:#x} - {:#x}", va, va + heap_size as usize);
    let usable = unsafe { hdr(va).size };
    kprintln!(
        "[HEAP] Usable: {} KiB ({} bytes) | Header: {} bytes",
        usable / 1024,
        usable,
        BLOCK_HEADER_SIZE
    );
    kprintln!("[HEAP] Initialized!");
}

/// Allocate `size` bytes (16-byte aligned). Returns the payload address, 0 on OOM.
pub fn kmalloc(size: usize) -> usize {
    if size == 0 {
        return 0;
    }
    if size > usize::MAX - (HEAP_ALIGN - 1) {
        uart::errorln("[HEAP] kmalloc: size overflow");
        return 0;
    }
    let size = align_up(size);
    let mut h = HEAP.lock();

    for _attempt in 0..2 {
        let mut current = h.head;
        while current != 0 {
            let (is_free, csize) = unsafe {
                let c = hdr(current);
                (c.is_free != 0, c.size)
            };
            if is_free && csize >= size {
                let remaining = csize - size;
                unsafe {
                    if remaining > BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE {
                        let new_addr = current + BLOCK_HEADER_SIZE + size;
                        let cnext = hdr(current).next;
                        let nb = hdr(new_addr);
                        nb.size = remaining - BLOCK_HEADER_SIZE;
                        nb.is_free = 1;
                        nb.magic = BLOCK_MAGIC_FREE;
                        nb.next = cnext;
                        let c = hdr(current);
                        c.size = size;
                        c.next = new_addr;
                    }
                    let c = hdr(current);
                    c.is_free = 0;
                    c.magic = BLOCK_MAGIC_ALLOC;
                }
                return current + BLOCK_HEADER_SIZE;
            }
            current = unsafe { hdr(current).next };
        }
        if !h.expand(size) {
            break;
        }
    }
    uart::errorln("[HEAP] kmalloc: out of memory!");
    0
}

pub fn kfree(ptr: usize) {
    if ptr == 0 {
        return;
    }
    let block = ptr - BLOCK_HEADER_SIZE;
    let h = HEAP.lock();

    if !h.addr_in_any_region(block) {
        uart::errorln("[HEAP] kfree: pointer outside heap regions!");
        return;
    }
    unsafe {
        let b = hdr(block);
        if b.magic != BLOCK_MAGIC_ALLOC {
            kprintln!(
                "[HEAP] kfree: bad magic {:#x} at {:#x} - refusing",
                b.magic,
                block
            );
            return;
        }
        if b.is_free != 0 {
            uart::errorln("[HEAP] kfree: double free detected!");
            return;
        }
        b.is_free = 1;
        b.magic = BLOCK_MAGIC_FREE;
    }

    // Coalesce physically-adjacent free blocks.
    let mut current = h.head;
    while current != 0 {
        unsafe {
            loop {
                let c = hdr(current);
                if c.is_free == 0 || c.next == 0 {
                    break;
                }
                let next = c.next;
                if hdr(next).is_free == 0 {
                    break;
                }
                let end_of_current = current + BLOCK_HEADER_SIZE + c.size;
                if end_of_current != next {
                    break; // gap — cannot merge
                }
                c.size += BLOCK_HEADER_SIZE + hdr(next).size;
                c.next = hdr(next).next;
            }
            current = hdr(current).next;
        }
    }
}

pub fn print_info() {
    let h = HEAP.lock();
    kprintln!("[HEAP][INFO] Heap block list:");
    let mut current = h.head;
    let mut total_free = 0u64;
    let mut total_used = 0u64;
    let mut count = 0u64;
    while current != 0 {
        let (size, is_free, next) = unsafe {
            let c = hdr(current);
            (c.size, c.is_free != 0, c.next)
        };
        kprintln!(
            "  [{}] addr={:#x} size={} {}",
            count,
            current,
            size,
            if is_free { "FREE" } else { "USED" }
        );
        if is_free {
            total_free += size as u64;
        } else {
            total_used += size as u64;
        }
        count += 1;
        current = next;
    }
    kprintln!(
        "[HEAP][INFO] Blocks: {} | Used: {} bytes | Free: {} bytes | Regions: {}",
        count,
        total_used,
        total_free,
        h.region_count
    );
}

pub fn used_bytes() -> u64 {
    let h = HEAP.lock();
    let mut total = 0u64;
    let mut c = h.head;
    while c != 0 {
        unsafe {
            let b = hdr(c);
            if b.is_free == 0 {
                total += b.size as u64;
            }
            c = b.next;
        }
    }
    total
}

pub fn free_bytes() -> u64 {
    let h = HEAP.lock();
    let mut total = 0u64;
    let mut c = h.head;
    while c != 0 {
        unsafe {
            let b = hdr(c);
            if b.is_free != 0 {
                total += b.size as u64;
            }
            c = b.next;
        }
    }
    total
}

pub fn total_bytes() -> u64 {
    let h = HEAP.lock();
    let mut total = 0u64;
    for i in 0..h.region_count as usize {
        total += h.regions[i].size_bytes;
    }
    total
}

pub fn run_tests() {
    kprintln!("[HEAP TEST] Running heap tests...");
    let tr = |name: &str, pass: bool| {
        kprintln!("[HEAP TEST] {}: {}", name, if pass { "PASS" } else { "FAIL" });
    };

    let a = kmalloc(8);
    tr("kmalloc returns non-null", a != 0);
    if a != 0 {
        unsafe { *(a as *mut u64) = 0xDEADBEEF };
        tr("write/read", unsafe { *(a as *const u64) } == 0xDEADBEEF);
    }
    let b = kmalloc(8);
    tr("different addresses", a != b);

    let buf = kmalloc(1024);
    tr("1KB alloc", buf != 0);
    if buf != 0 {
        unsafe { core::ptr::write_bytes(buf as *mut u8, b'A', 1024) };
        let ok = unsafe { *(buf as *const u8) == b'A' && *((buf + 1023) as *const u8) == b'A' };
        tr("1KB write/read", ok);
    }

    kfree(a);
    kfree(b);
    let c = kmalloc(8);
    tr("free/reuse", c == a || c == b);

    kfree(buf);
    kfree(c);
    let buf2 = kmalloc(2048);
    tr("coalesce + realloc", buf2 != 0);
    kfree(buf2);

    print_info();
    kprintln!("[HEAP TEST] Done!");
}

// ---------------------------------------------------------------------------
// Global allocator backing alloc::{Vec, Box, String}.
// kmalloc returns 16-byte-aligned payloads. For larger alignments we
// over-allocate and stash the real base pointer just before the aligned one.
// ---------------------------------------------------------------------------

pub struct KernelAlloc;

unsafe impl GlobalAlloc for KernelAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        if align <= HEAP_ALIGN {
            kmalloc(layout.size()) as *mut u8
        } else {
            let size = layout.size() + align + core::mem::size_of::<usize>();
            let base = kmalloc(size);
            if base == 0 {
                return core::ptr::null_mut();
            }
            let raw = base + core::mem::size_of::<usize>();
            let aligned = (raw + align - 1) & !(align - 1);
            *((aligned - core::mem::size_of::<usize>()) as *mut usize) = base;
            aligned as *mut u8
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= HEAP_ALIGN {
            kfree(ptr as usize);
        } else {
            let base = *((ptr as usize - core::mem::size_of::<usize>()) as *const usize);
            kfree(base);
        }
    }
}

#[global_allocator]
static GLOBAL: KernelAlloc = KernelAlloc;
