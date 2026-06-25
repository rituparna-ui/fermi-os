//! Physical Memory Manager — bitmap page allocator.
//!
//! Direct port of the original `src/mm/pmm/pmm.c`. Manages 8 GiB of RAM at the
//! QEMU `virt` base with single- and contiguous-multi-page allocation. The
//! bitmap is placed immediately after the kernel image (`__kernel_end`).
//!
//! NOTE: this subsystem runs **before the MMU is enabled**, so it must log via
//! the simple `uart` helpers (aligned byte stores). `core::fmt` (kprintln!)
//! does unaligned digit-pair copies that fault on Device memory.

use crate::uart;
use crate::sync::Racy;

pub const MEM_START: u64 = 0x4000_0000;
pub const MEM_SIZE: u64 = 8 * 1024 * 1024 * 1024;
pub const PAGE_SIZE: u64 = 4096;
pub const PAGE_SHIFT: u64 = 12;

#[inline(always)]
pub const fn pfn_to_phys(pfn: u64) -> u64 {
    pfn << PAGE_SHIFT
}
#[inline(always)]
pub const fn phys_to_pfn(addr: u64) -> u64 {
    addr >> PAGE_SHIFT
}
#[inline(always)]
pub const fn page_align_up(addr: u64) -> u64 {
    (addr + PAGE_SIZE - 1) & !(PAGE_SIZE - 1)
}
#[inline(always)]
pub const fn page_align_down(addr: u64) -> u64 {
    addr & !(PAGE_SIZE - 1)
}

extern "C" {
    static __kernel_end: u8;
}

struct Pmm {
    bitmap: usize, // *mut u64, stored as usize to be Send
    bitmap_words: u64,
    total_pages: u64,
    used_pages: u64,
    reserved_pages: u64,
    region_start: u64,
    region_end: u64,
}

impl Pmm {
    const fn empty() -> Self {
        Self {
            bitmap: 0,
            bitmap_words: 0,
            total_pages: 0,
            used_pages: 0,
            reserved_pages: 0,
            region_start: 0,
            region_end: 0,
        }
    }

    #[inline(always)]
    fn word(&self, i: u64) -> *mut u64 {
        (self.bitmap as *mut u64).wrapping_add(i as usize)
    }
    #[inline(always)]
    fn set(&mut self, pfn: u64) {
        unsafe { *self.word(pfn / 64) |= 1u64 << (pfn % 64) };
    }
    #[inline(always)]
    fn clear(&mut self, pfn: u64) {
        unsafe { *self.word(pfn / 64) &= !(1u64 << (pfn % 64)) };
    }
    #[inline(always)]
    fn test(&self, pfn: u64) -> bool {
        unsafe { (*self.word(pfn / 64) >> (pfn % 64)) & 1 != 0 }
    }
    #[inline(always)]
    fn read_word(&self, i: u64) -> u64 {
        unsafe { *self.word(i) }
    }
}

static PMM: Racy<Pmm> = Racy::new(Pmm::empty());

pub fn init(mem_start: u64, mem_size: u64) {
    uart::println("[PMM] Initializing Physical Memory Manager");
    let p = unsafe { PMM.get() };
    p.region_start = mem_start;
    p.region_end = mem_start + mem_size;
    p.total_pages = mem_size / PAGE_SIZE;

    p.bitmap_words = (p.total_pages + 63) / 64;
    let bitmap_bytes = p.bitmap_words * 8;
    uart::log_dec("[PMM] Bitmap Length: ", p.bitmap_words);
    uart::log_dec("[PMM] Bitmap Bytes: ", bitmap_bytes);

    let kernel_end = unsafe { &__kernel_end as *const u8 as u64 };
    p.bitmap = page_align_up(kernel_end) as usize;
    uart::log_hex("[PMM] Bitmap address: ", p.bitmap as u64);
    uart::log_hex("[PMM] Kernel End: ", kernel_end);

    uart::println("[PMM] Zeroing Bitmap");
    unsafe {
        core::ptr::write_bytes(p.bitmap as *mut u8, 0, bitmap_bytes as usize);
    }

    // Mark non-existent high bits of the final word as used.
    if p.total_pages % 64 != 0 {
        let real_bits = p.total_pages % 64;
        let last = p.bitmap_words - 1;
        unsafe { *p.word(last) = !((1u64 << real_bits) - 1) };
    }

    uart::println("[PMM] Mark kernel and bitmap space reserved");
    let bitmap_end = p.bitmap as u64 + bitmap_bytes;
    let reserved_end = page_align_up(bitmap_end);
    p.reserved_pages = (reserved_end - p.region_start) / PAGE_SIZE;
    uart::log_hex("[PMM] Bitmap End: ", reserved_end);

    for pfn in 0..p.reserved_pages {
        p.set(pfn);
    }
    p.used_pages = p.reserved_pages;
    uart::println("[PMM] Initialized!");
}

/// After the higher-half jump, relocate the bitmap pointer from its physical
/// address to the TTBR1 upper-half alias so it stays mapped when TTBR0 is
/// swapped for a user task.
pub fn relocate_upper() {
    let p = unsafe { PMM.get() };
    p.bitmap = crate::mm::mmu::phys_to_virt(p.bitmap as u64) as usize;
    uart::log_hex("[PMM] Bitmap relocated to upper half: ", p.bitmap as u64);
}

pub fn print_info() {
    let p = unsafe { PMM.get() };
    let mem_size = p.region_end - p.region_start;
    uart::puts("[PMM][INFO] Memory region: ");
    uart::puthex(p.region_start);
    uart::puts(" - ");
    uart::puthex(p.region_end);
    uart::putc(b'\n');
    uart::log_dec("[PMM][INFO] Memory Size (MiB): ", mem_size / 1024 / 1024);
    uart::log_dec("[PMM][INFO] Total Pages: ", p.total_pages);
    uart::log_dec("[PMM][INFO] Reserved Pages: ", p.reserved_pages);
    uart::log_dec("[PMM][INFO] Used Pages: ", p.used_pages);
    uart::log_dec("[PMM][INFO] Free Pages: ", p.total_pages - p.used_pages);
}

/// Allocate one physical page. Returns 0 on OOM (matches the C convention).
pub fn allocate_page() -> u64 {
    let p = unsafe { PMM.get() };
    for i in 0..p.bitmap_words {
        if p.read_word(i) == u64::MAX {
            continue;
        }
        for bit in 0..64u64 {
            let pfn = i * 64 + bit;
            if pfn >= p.total_pages {
                uart::errorln("[PMM] Out of range pfn.");
                return 0;
            }
            if !p.test(pfn) {
                p.set(pfn);
                p.used_pages += 1;
                return p.region_start + pfn_to_phys(pfn);
            }
        }
    }
    uart::errorln("[PMM] Out of memory! No free pages available.");
    0
}

/// Allocate `count` contiguous physical pages. Returns 0 on failure.
pub fn allocate_pages(count: u64) -> u64 {
    if count == 0 {
        return 0;
    }
    if count == 1 {
        return allocate_page();
    }

    let p = unsafe { PMM.get() };
    let mut run_start = 0u64;
    let mut run_len = 0u64;
    for pfn in p.reserved_pages..p.total_pages {
        if !p.test(pfn) {
            if run_len == 0 {
                run_start = pfn;
            }
            run_len += 1;
            if run_len == count {
                for i in 0..count {
                    p.set(run_start + i);
                }
                p.used_pages += count;
                return p.region_start + pfn_to_phys(run_start);
            }
        } else {
            run_len = 0;
        }
    }
    uart::errorln("[PMM] No contiguous block found!");
    0
}

pub fn free_page(phys_addr: u64) {
    let p = unsafe { PMM.get() };
    if phys_addr < p.region_start || phys_addr >= p.region_end {
        uart::errorln("[PMM] address outside managed region");
        return;
    }
    if phys_addr & (PAGE_SIZE - 1) != 0 {
        uart::errorln("[PMM] non page aligned address");
        return;
    }
    let pfn = phys_to_pfn(phys_addr - p.region_start);
    if pfn < p.reserved_pages {
        uart::errorln("[PMM] reserved page");
        return;
    }
    if !p.test(pfn) {
        uart::errorln("[PMM] unallocated page");
        return;
    }
    p.clear(pfn);
    p.used_pages -= 1;
}

pub fn free_pages(phys_addr: u64, count: u64) {
    for i in 0..count {
        free_page(phys_addr + i * PAGE_SIZE);
    }
}

pub fn total_pages() -> u64 {
    unsafe { PMM.get() }.total_pages
}
pub fn used_pages() -> u64 {
    unsafe { PMM.get() }.used_pages
}
pub fn free_pages_count() -> u64 {
    let p = unsafe { PMM.get() };
    p.total_pages - p.used_pages
}
pub fn reserved_pages() -> u64 {
    unsafe { PMM.get() }.reserved_pages
}
