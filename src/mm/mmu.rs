//! Memory Management Unit — 3-level page tables, 4 KiB granule, 48-bit VA.
//!
//! Direct port of the original `src/mm/mmu/mmu.c`.
//!
//! Stage 1 of the port enables the MMU with an identity map for TTBR0 (lower
//! half) and a `+KERNEL_VA_OFFSET` map for TTBR1 (upper half). The kernel keeps
//! executing at its low identity VA. Once the MMU is on, RAM is Normal
//! cacheable memory, so `kprintln!` and `SpinLock` become safe to use.

use crate::mrs;
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::sync::Racy;
use crate::uart;

// Page-table entry bits.
pub const PTE_VALID: u64 = 1 << 0;
pub const PTE_TABLE: u64 = 1 << 1;
pub const PTE_BLOCK: u64 = 0 << 1;
pub const PTE_AF: u64 = 1 << 10;
pub const PTE_SH_INNER: u64 = 3 << 8;
pub const PTE_AP_RW: u64 = 0 << 6; // EL1 RW, EL0 none
pub const PTE_AP_RW_EL0: u64 = 1 << 6; // EL1 RW, EL0 RW
pub const PTE_AP_RO: u64 = 2 << 6; // EL1 RO, EL0 none
pub const PTE_AP_RO_EL0: u64 = 3 << 6; // EL1 RO, EL0 RO
pub const PTE_UXN: u64 = 1 << 54;
pub const PTE_PXN: u64 = 1 << 53;
pub const PTE_NG: u64 = 1 << 11;

#[inline(always)]
pub const fn pte_attridx(idx: u64) -> u64 {
    idx << 2
}

pub const SZ_512GB: u64 = 0x80_0000_0000;
pub const SZ_1GB: u64 = 0x4000_0000;
pub const SZ_2MB: u64 = 0x20_0000;

pub const KERNEL_VA_OFFSET: u64 = 0xFFFF_0000_0000_0000;

#[inline(always)]
pub const fn phys_to_virt(pa: u64) -> u64 {
    pa + KERNEL_VA_OFFSET
}
#[inline(always)]
pub const fn virt_to_phys(va: u64) -> u64 {
    va - KERNEL_VA_OFFSET
}

#[inline(always)]
pub const fn l0_index(va: u64) -> u64 {
    (va >> 39) & 0x1FF
}
#[inline(always)]
pub const fn l1_index(va: u64) -> u64 {
    (va >> 30) & 0x1FF
}
#[inline(always)]
pub const fn l2_index(va: u64) -> u64 {
    (va >> 21) & 0x1FF
}
#[inline(always)]
pub const fn l3_index(va: u64) -> u64 {
    (va >> 12) & 0x1FF
}

pub const PTE_ADDR_MASK: u64 = 0x0000_FFFF_FFFF_F000;

// ASID / TTBR packing.
pub const TTBR_ASID_SHIFT: u64 = 48;
pub const TTBR_BADDR_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;

#[inline(always)]
pub const fn ttbr_pack(baddr: u64, asid: u16) -> u64 {
    (baddr & TTBR_BADDR_MASK) | ((asid as u64) << TTBR_ASID_SHIFT)
}
#[inline(always)]
pub const fn ttbr_baddr(ttbr: u64) -> u64 {
    ttbr & TTBR_BADDR_MASK
}

// User-space address layout (TTBR0).
pub const USER_TEXT_BASE: u64 = 0x0040_0000;
pub const USER_STACK_TOP: u64 = 0x0080_0000;
pub const USER_STACK_PAGES: u64 = 4;

#[inline(always)]
fn pte_next_table(entry: u64) -> u64 {
    entry & PTE_ADDR_MASK
}
#[inline(always)]
fn pte_valid(entry: u64) -> bool {
    entry & PTE_VALID != 0
}

struct MmuState {
    l0_lo: u64,
    l0_hi: u64,
}
static MMU: Racy<MmuState> = Racy::new(MmuState { l0_lo: 0, l0_hi: 0 });

#[inline(always)]
unsafe fn read_pte(table_phys: u64, idx: u64) -> u64 {
    let p = table_ptr(table_phys).add(idx as usize);
    *p
}
#[inline(always)]
unsafe fn write_pte(table_phys: u64, idx: u64, val: u64) {
    let p = table_ptr(table_phys).add(idx as usize);
    *p = val;
}

/// Resolve a table's physical address to a dereferenceable pointer.
/// Before the MMU is enabled, physical == virtual. After, route through the
/// TTBR1 upper half so the access works regardless of the active TTBR0.
#[inline(always)]
fn table_ptr(table_phys: u64) -> *mut u64 {
    let sctlr: u64 = mrs!(sctlr_el1);
    let va = if sctlr & 1 != 0 {
        phys_to_virt(table_phys)
    } else {
        table_phys
    };
    va as *mut u64
}

/// Allocate and zero a fresh page-table page; returns its physical address.
fn alloc_table() -> u64 {
    let phys = pmm::allocate_page();
    if phys == 0 {
        uart::errorln("[MMU] Failed to allocate table");
        return 0;
    }
    let va = table_ptr(phys);
    unsafe { core::ptr::write_bytes(va as *mut u8, 0, PAGE_SIZE as usize) };
    phys
}

/// Build an L0->L1->L2 identity hierarchy mapping the first 1 TiB with 2 MiB
/// blocks. Returns the L0 physical address (0 on failure). `out_l1_lo` receives
/// the low L1 table phys (used by the L2 remap test).
fn build_identity_tables(out_l1_lo: Option<&mut u64>) -> u64 {
    let l0 = alloc_table();
    if l0 == 0 {
        return 0;
    }
    let l1 = alloc_table();
    if l1 == 0 {
        return 0;
    }
    unsafe { write_pte(l0, 0, l1 | PTE_VALID | PTE_TABLE) };

    let mem_end = pmm::MEM_START + pmm::MEM_SIZE;
    for l1i in 0..512u64 {
        let l2 = alloc_table();
        if l2 == 0 {
            return 0;
        }
        unsafe { write_pte(l1, l1i, l2 | PTE_VALID | PTE_TABLE) };
        for l2i in 0..512u64 {
            let phys = l1i * SZ_1GB + l2i * SZ_2MB;
            if phys == 0 {
                continue; // leave null page unmapped so deref faults
            }
            let attr = if phys >= pmm::MEM_START && phys < mem_end {
                1
            } else {
                0
            };
            let pte = phys
                | PTE_VALID
                | PTE_BLOCK
                | PTE_AF
                | PTE_SH_INNER
                | PTE_AP_RW
                | pte_attridx(attr);
            unsafe { write_pte(l2, l2i, pte) };
        }
    }

    // L0[1] -> 512 GiB..1 TiB (PCI MMIO64 window, all device memory).
    let l1_hi = alloc_table();
    if l1_hi == 0 {
        return 0;
    }
    unsafe { write_pte(l0, 1, l1_hi | PTE_VALID | PTE_TABLE) };
    for l1i in 0..512u64 {
        let l2 = alloc_table();
        if l2 == 0 {
            return 0;
        }
        unsafe { write_pte(l1_hi, l1i, l2 | PTE_VALID | PTE_TABLE) };
        for l2i in 0..512u64 {
            let phys = SZ_512GB + l1i * SZ_1GB + l2i * SZ_2MB;
            let pte = phys
                | PTE_VALID
                | PTE_BLOCK
                | PTE_AF
                | PTE_SH_INNER
                | PTE_AP_RW
                | pte_attridx(0);
            unsafe { write_pte(l2, l2i, pte) };
        }
    }

    if let Some(slot) = out_l1_lo {
        *slot = l1;
    }
    l0
}

/// Initialise and enable the MMU. Returns the low L1 table phys (for tests).
pub fn init() -> u64 {
    uart::println("[MMU] Initializing MMU (48 bit VAS, 4kb granule)");

    // MAIR: AttrIdx0 = Device-nGnRnE (0x00), AttrIdx1 = Normal WB (0xFF).
    let mair: u64 = (0x00 << 0) | (0xFF << 8);
    crate::msr!(mair_el1, mair);

    let mut l1_lo: u64 = 0;
    let l0_lo = build_identity_tables(Some(&mut l1_lo));
    if l0_lo == 0 {
        uart::errorln("[MMU] Failed to build TTBR0 tables");
        return 0;
    }
    uart::println("[MMU] TTBR0 lower half tables built");

    let l0_hi = build_identity_tables(None);
    if l0_hi == 0 {
        uart::errorln("[MMU] Failed to build TTBR1 tables");
        return 0;
    }
    uart::println("[MMU] TTBR1 upper half tables built");

    let st = unsafe { MMU.get() };
    st.l0_lo = l0_lo;
    st.l0_hi = l0_hi;

    let tcr: u64 = (16 << 0)        // T0SZ=16 -> 48-bit VA
        | (0b01 << 8)               // IRGN0 WB-WA
        | (0b01 << 10)              // ORGN0 WB-WA
        | (0b11 << 12)              // SH0 inner shareable
        | (0b00 << 14)              // TG0 4KB
        | (16 << 16)                // T1SZ=16
        | (0b01 << 24)              // IRGN1 WB-WA
        | (0b01 << 26)              // ORGN1 WB-WA
        | (0b11 << 28)              // SH1 inner shareable
        | (0b10 << 30)              // TG1 4KB
        | (0b010 << 32)             // IPS 40-bit PA
        | (1 << 36); // AS=1 -> 16-bit ASIDs
    crate::msr!(tcr_el1, tcr);
    unsafe { core::arch::asm!("dsb ish") };

    crate::msr!(ttbr0_el1, l0_lo);
    crate::msr!(ttbr1_el1, l0_hi);
    unsafe {
        core::arch::asm!("dsb ish", "isb");
        core::arch::asm!("tlbi vmalle1", "dsb ish", "isb");
    }

    // Enable MMU + caches.
    let mut sctlr: u64 = mrs!(sctlr_el1);
    sctlr |= 1 << 0; // M
    sctlr |= 1 << 2; // C
    sctlr |= 1 << 12; // I
    crate::msr!(sctlr_el1, sctlr);
    unsafe { core::arch::asm!("isb") };

    uart::println("[MMU] Enabled");
    l1_lo
}

/// Create an empty L0 table for a user address space (TTBR0). Tables for
/// L1/L2/L3 are allocated on demand by [`map_user_range`].
pub fn create_user_tables() -> u64 {
    alloc_table()
}

/// Walk the L0->L1->L2->L3 tables, allocating intermediate pages on demand
/// when `alloc` is true. Returns a pointer to the entry at `target_level`
/// (2 = L2, 3 = L3), routed through the TTBR1 upper half. Null on failure.
fn walk_levels(l0_phys: u64, va: u64, target_level: i32, alloc: bool) -> *mut u64 {
    let l0i = l0_index(va);
    let l1i = l1_index(va);
    let l2i = l2_index(va);
    let l3i = l3_index(va);

    // L0 -> L1
    let l1_phys = unsafe {
        let e = read_pte(l0_phys, l0i);
        if !pte_valid(e) {
            if !alloc {
                return core::ptr::null_mut();
            }
            let n = alloc_table();
            if n == 0 {
                return core::ptr::null_mut();
            }
            write_pte(l0_phys, l0i, n | PTE_VALID | PTE_TABLE);
            n
        } else {
            pte_next_table(e)
        }
    };

    // L1 -> L2
    let l2_phys = unsafe {
        let e = read_pte(l1_phys, l1i);
        if !pte_valid(e) {
            if !alloc {
                return core::ptr::null_mut();
            }
            let n = alloc_table();
            if n == 0 {
                return core::ptr::null_mut();
            }
            write_pte(l1_phys, l1i, n | PTE_VALID | PTE_TABLE);
            n
        } else {
            pte_next_table(e)
        }
    };

    if target_level == 2 {
        return table_ptr(l2_phys).wrapping_add(l2i as usize);
    }

    // L2 -> L3
    let l3_phys = unsafe {
        let e = read_pte(l2_phys, l2i);
        if !pte_valid(e) {
            if !alloc {
                return core::ptr::null_mut();
            }
            let n = alloc_table();
            if n == 0 {
                return core::ptr::null_mut();
            }
            write_pte(l2_phys, l2i, n | PTE_VALID | PTE_TABLE);
            n
        } else {
            pte_next_table(e)
        }
    };

    table_ptr(l3_phys).wrapping_add(l3i as usize)
}

/// Map `pages` contiguous 4 KiB pages [pa, pa+pages*PAGE_SIZE) at user VA `va`.
pub fn map_user_range(l0_phys: u64, va: u64, pa: u64, pages: u64, flags: u64) {
    for i in 0..pages {
        let pte = walk_levels(l0_phys, va + i * PAGE_SIZE, 3, true);
        if pte.is_null() {
            uart::errorln("[MMU] map_user_range: walk failed");
            return;
        }
        unsafe {
            *pte = ((pa + i * PAGE_SIZE) & PTE_ADDR_MASK)
                | PTE_VALID
                | PTE_TABLE
                | PTE_AF
                | PTE_SH_INNER
                | PTE_NG
                | flags;
        }
    }
}

/// Free all page-table pages (L0..L3) of a user address space. The L3-mapped
/// data pages (user text/stack) are freed separately by the scheduler.
pub fn free_user_tables(l0_phys: u64) {
    for i in 0..512u64 {
        let l0e = unsafe { read_pte(l0_phys, i) };
        if !pte_valid(l0e) {
            continue;
        }
        let l1_phys = pte_next_table(l0e);
        for j in 0..512u64 {
            let l1e = unsafe { read_pte(l1_phys, j) };
            if !pte_valid(l1e) || l1e & PTE_TABLE == 0 {
                continue;
            }
            let l2_phys = pte_next_table(l1e);
            for k in 0..512u64 {
                let l2e = unsafe { read_pte(l2_phys, k) };
                if !pte_valid(l2e) || l2e & PTE_TABLE == 0 {
                    continue;
                }
                pmm::free_page(pte_next_table(l2e));
            }
            pmm::free_page(l2_phys);
        }
        pmm::free_page(l1_phys);
    }
    pmm::free_page(l0_phys);
}

// ---------------------------------------------------------------------------
// Self-tests (run right after enable, while TTBR0 = boot identity map).
// ---------------------------------------------------------------------------

fn print_result(name: &str, pass: bool) {
    crate::kprintln!("[MMU TEST] {}: {}", name, if pass { "PASS" } else { "FAIL" });
}

fn test_mmu_enabled() -> bool {
    let sctlr: u64 = mrs!(sctlr_el1);
    sctlr & 1 != 0
}

fn test_identity_mapping() -> bool {
    let page = pmm::allocate_page();
    if page == 0 {
        return false;
    }
    let ptr = page as *mut u64;
    unsafe {
        *ptr = 0xAABBCCDD;
    }
    let pass = unsafe { *ptr == 0xAABBCCDD };
    pmm::free_page(page);
    pass
}

fn test_ttbr1_upper_half() -> bool {
    let pa = pmm::allocate_page();
    if pa == 0 {
        return false;
    }
    let lo = pa as *mut u64;
    unsafe {
        *lo = 0xABCDEFAD;
        core::arch::asm!("dsb ish");
    }
    let hi = phys_to_virt(pa) as *mut u64;
    let mut pass = unsafe { *hi == 0xABCDEFAD };
    unsafe {
        *hi = 0xABBCCCDD;
        core::arch::asm!("dsb ish");
        pass &= *lo == 0xABBCCCDD;
    }
    pmm::free_page(pa);
    pass
}

pub fn run_tests(_l1_lo_phys: u64) {
    print_result("MMU Enabled", test_mmu_enabled());
    print_result("Identity Mapping", test_identity_mapping());
    print_result("TTBR1 Upper Half", test_ttbr1_upper_half());
}
