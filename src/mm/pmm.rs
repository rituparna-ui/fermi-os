//! Physical Memory Manager — a bitmap page allocator.
//!
//! Manages the 8 GiB of RAM the QEMU `virt` machine exposes starting at
//! physical `0x4000_0000`. One bit per 4 KiB page: set = used/reserved, clear =
//! free. The bitmap itself lives in RAM immediately after the kernel image and
//! is reserved along with the kernel.
//!
//! Lifecycle (mirrors the C original):
//!   1. `init(MEM_START, MEM_SIZE)` — pre-MMU, identity mapped. Places and zeros
//!      the bitmap, reserves kernel + bitmap pages.
//!   2. `relocate_upper()` — once after the MMU is live: re-point the bitmap at
//!      its higher-half virtual address.
//!   3. `allocate_page`/`allocate_pages`/`free_page`/`free_pages` thereafter.
//!
//! All boot-time logging uses the UART's aligned helpers: `init` runs before
//! the MMU maps RAM as Normal memory, so `core::fmt` would fault.

use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;

pub const MEM_START: u64 = 0x4000_0000;
pub const MEM_SIZE: u64 = 8 * 1024 * 1024 * 1024;
pub const PAGE_SIZE: u64 = 4096;
pub const PAGE_SHIFT: u32 = 12;

/// Higher-half offset applied to physical addresses to reach the kernel's
/// virtual mapping (TTBR1). Matches `KERNEL_VA_OFFSET` in the C MMU code.
pub const KERNEL_VA_OFFSET: u64 = 0xFFFF_0000_0000_0000;

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

extern "C" {
    /// Defined by the linker script: first byte past the kernel image + stack.
    static __kernel_end: u8;
}

/// All mutable PMM state, guarded by a single spin lock.
struct Pmm {
    /// Address of the bitmap (physical pre-relocation, virtual after).
    bitmap: *mut u64,
    /// Number of `u64` words in the bitmap.
    bitmap_size: u64,
    total_pages: u64,
    used_pages: u64,
    reserved_pages: u64,
    mem_region_start: u64,
    mem_region_end: u64,
}

// SAFETY: the raw bitmap pointer is only dereferenced under the SpinLock.
unsafe impl Send for Pmm {}

impl Pmm {
    const fn new() -> Self {
        Self {
            bitmap: core::ptr::null_mut(),
            bitmap_size: 0,
            total_pages: 0,
            used_pages: 0,
            reserved_pages: 0,
            mem_region_start: 0,
            mem_region_end: 0,
        }
    }

    #[inline]
    fn bitmap_set(&mut self, pfn: u64) {
        let idx = (pfn / 64) as usize;
        let bit = pfn % 64;
        unsafe {
            let cell = self.bitmap.add(idx);
            cell.write(cell.read() | (1u64 << bit));
        }
    }

    #[inline]
    fn bitmap_clear(&mut self, pfn: u64) {
        let idx = (pfn / 64) as usize;
        let bit = pfn % 64;
        unsafe {
            let cell = self.bitmap.add(idx);
            cell.write(cell.read() & !(1u64 << bit));
        }
    }

    #[inline]
    fn bitmap_test(&self, pfn: u64) -> bool {
        let idx = (pfn / 64) as usize;
        let bit = pfn % 64;
        unsafe { (self.bitmap.add(idx).read() >> bit) & 1 == 1 }
    }

    #[inline]
    fn bitmap_word(&self, idx: u64) -> u64 {
        unsafe { self.bitmap.add(idx as usize).read() }
    }
}

static PMM: SpinLock<Pmm> = SpinLock::new(Pmm::new());

/// Initialize the PMM over `[mem_start, mem_start + mem_size)`. Pre-MMU.
pub fn init(mem_start: u64, mem_size: u64) {
    let uart = Uart;
    uart.println("[PMM] Initializing Physical Memory Manager");

    let mut pmm = PMM.lock();
    pmm.mem_region_start = mem_start;
    pmm.mem_region_end = mem_start + mem_size;
    pmm.total_pages = mem_size / PAGE_SIZE;

    // One bit per page: ceil(total_pages / 64) words.
    pmm.bitmap_size = (pmm.total_pages + 63) / 64;
    let bitmap_bytes = pmm.bitmap_size * core::mem::size_of::<u64>() as u64;

    uart.puts("[PMM] Bitmap Length: ");
    uart.putdec(pmm.bitmap_size);
    uart.putc(b'\n');
    uart.puts("[PMM] Bitmap Bytes: ");
    uart.putdec(bitmap_bytes);
    uart.putc(b'\n');

    // Place the bitmap at the first page-aligned address after the kernel.
    let kernel_end = core::ptr::addr_of!(__kernel_end) as u64;
    let bitmap_addr = page_align_up(kernel_end);
    pmm.bitmap = bitmap_addr as *mut u64;

    uart.puts("[PMM] Bitmap address: ");
    uart.puthex(bitmap_addr);
    uart.putc(b'\n');
    uart.puts("[PMM] Kernel End: ");
    uart.puthex(kernel_end);
    uart.putc(b'\n');

    uart.println("[PMM] Zeroing Bitmap");
    unsafe {
        core::ptr::write_bytes(pmm.bitmap as *mut u8, 0, bitmap_bytes as usize);
    }

    // If total_pages isn't a multiple of 64, the high bits of the last word map
    // to pages that don't exist. Pre-mark them used so the allocator never
    // returns an out-of-range PFN and the "skip full word" fast path holds.
    if pmm.total_pages % 64 != 0 {
        let real_bits = pmm.total_pages % 64;
        let last = pmm.bitmap_size - 1;
        unsafe {
            pmm.bitmap
                .add(last as usize)
                .write(!((1u64 << real_bits) - 1));
        }
    }

    uart.println("[PMM] Mark kernel and bitmap space reserved");

    // Reserve kernel image + stack + bitmap.
    let bitmap_end = bitmap_addr + bitmap_bytes;
    let reserved_end = page_align_up(bitmap_end);
    pmm.reserved_pages = (reserved_end - pmm.mem_region_start) / PAGE_SIZE;

    uart.puts("[PMM] Bitmap End: ");
    uart.puthex(reserved_end);
    uart.putc(b'\n');

    let reserved = pmm.reserved_pages;
    for pfn in 0..reserved {
        pmm.bitmap_set(pfn);
    }
    pmm.used_pages = reserved;

    uart.println("[PMM] Initialized!");
}

/// Re-point the bitmap at its higher-half virtual address. Call exactly once,
/// after the MMU is live.
pub fn relocate_upper() {
    let mut pmm = PMM.lock();
    let virt = phys_to_virt(pmm.bitmap as u64);
    pmm.bitmap = virt as *mut u64;
    drop(pmm);

    // Post-MMU: RAM is Normal memory, so core::fmt is safe here.
    kprintln!("[PMM] Bitmap relocated to upper half: {:#x}", virt);
}

/// Allocate a single 4 KiB page. Returns its physical address, or 0 on failure.
pub fn allocate_page() -> u64 {
    let mut pmm = PMM.lock();
    let bitmap_size = pmm.bitmap_size;
    let total_pages = pmm.total_pages;
    let mem_region_start = pmm.mem_region_start;

    for i in 0..bitmap_size {
        if pmm.bitmap_word(i) == u64::MAX {
            continue; // word fully used
        }
        for bit in 0..64u64 {
            let pfn = i * 64 + bit;
            if pfn >= total_pages {
                Uart.errorln("[PMM] Out of range pfm.");
                return 0;
            }
            if !pmm.bitmap_test(pfn) {
                pmm.bitmap_set(pfn);
                pmm.used_pages += 1;
                return mem_region_start + pfn_to_phys(pfn);
            }
        }
    }

    Uart.errorln("[PMM] Out of memory! No free pages available.");
    0
}

/// Allocate `count` contiguous pages. Returns the physical base, or 0 on
/// failure.
pub fn allocate_pages(count: u64) -> u64 {
    if count == 0 {
        return 0;
    }
    if count == 1 {
        return allocate_page();
    }

    let mut pmm = PMM.lock();
    let total_pages = pmm.total_pages;
    let reserved_pages = pmm.reserved_pages;
    let mem_region_start = pmm.mem_region_start;

    let mut run_start = 0u64;
    let mut run_length = 0u64;

    for pfn in reserved_pages..total_pages {
        if !pmm.bitmap_test(pfn) {
            if run_length == 0 {
                run_start = pfn;
            }
            run_length += 1;
            if run_length == count {
                for i in 0..count {
                    pmm.bitmap_set(run_start + i);
                }
                pmm.used_pages += count;
                return mem_region_start + pfn_to_phys(run_start);
            }
        } else {
            run_length = 0;
        }
    }

    Uart.errorln("[PMM] No contiguous block found!");
    0
}

/// Free a single page (must be page-aligned, in range, allocated, non-reserved).
pub fn free_page(phys_addr: u64) {
    let mut pmm = PMM.lock();

    if phys_addr < pmm.mem_region_start || phys_addr >= pmm.mem_region_end {
        Uart.errorln("[PMM] address outside managed region");
        return;
    }
    if phys_addr & (PAGE_SIZE - 1) != 0 {
        Uart.errorln("[PMM] non page aligned address");
        return;
    }

    let pfn = phys_to_pfn(phys_addr - pmm.mem_region_start);
    if pfn < pmm.reserved_pages {
        Uart.errorln("[PMM] reserved page");
        return;
    }
    if !pmm.bitmap_test(pfn) {
        Uart.errorln("[PMM] unallocated page");
        return;
    }

    pmm.bitmap_clear(pfn);
    pmm.used_pages -= 1;
}

/// Free `count` contiguous pages starting at `phys_addr`.
pub fn free_pages(phys_addr: u64, count: u64) {
    for i in 0..count {
        free_page(phys_addr + i * PAGE_SIZE);
    }
}

pub fn total_pages() -> u64 {
    PMM.lock().total_pages
}

pub fn used_pages() -> u64 {
    PMM.lock().used_pages
}

pub fn free_pages_count() -> u64 {
    let pmm = PMM.lock();
    pmm.total_pages - pmm.used_pages
}

pub fn reserved_pages() -> u64 {
    PMM.lock().reserved_pages
}

/// Print PMM statistics over the UART (aligned helpers; pre-MMU safe).
pub fn print_info() {
    let pmm = PMM.lock();
    let uart = Uart;
    let mem_size = pmm.mem_region_end - pmm.mem_region_start;

    uart.puts("[PMM][INFO] Memory region: ");
    uart.puthex(pmm.mem_region_start);
    uart.puts(" - ");
    uart.puthex(pmm.mem_region_end);
    uart.putc(b'\n');

    uart.puts("[PMM][INFO] Memory Size: ");
    uart.puthex(mem_size);
    uart.puts(" | ");
    uart.putdec(mem_size / 1024 / 1024);
    uart.puts(" mbytes\n");

    uart.puts("[PMM][INFO] Total Pages: ");
    uart.putdec(pmm.total_pages);
    uart.putc(b'\n');
    uart.puts("[PMM][INFO] Reserved Pages: ");
    uart.putdec(pmm.reserved_pages);
    uart.putc(b'\n');
    uart.puts("[PMM][INFO] Used Pages: ");
    uart.putdec(pmm.used_pages);
    uart.putc(b'\n');
    uart.puts("[PMM][INFO] Free Pages: ");
    uart.putdec(pmm.total_pages - pmm.used_pages);
    uart.putc(b'\n');
}
