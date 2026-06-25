//! Memory Management Unit — 4 KiB granule, 48-bit VA, 4-level (L0→L1→L2→L3).
//!
//! `init` (called pre-MMU from `early_init`) builds two identity table trees —
//! TTBR0 for the lower half and TTBR1 for the upper half — using 2 MiB L2
//! blocks for the first 1 TiB of PA space, programs MAIR/TCR/TTBR, and enables
//! the MMU. Per-task user address spaces are built lazily at L3 (4 KiB pages)
//! via `create_user_tables` + `map_user_range`, and torn down by
//! `free_user_tables`.
//!
//! Page-table contents (PTEs) hold physical addresses. After the MMU is on,
//! every table dereference is routed through the upper-half kernel mapping
//! (`phys_to_virt`, TTBR1) so it works regardless of which task's sparse TTBR0
//! is currently loaded.

use crate::klib::uart::Uart;
use crate::mm::consts::*;
use crate::mm::pmm;
use crate::{kprintln, mrs, msr};

#[allow(unused_imports)]
pub use crate::mm::consts::{USER_STACK_PAGES, USER_STACK_TOP, USER_TEXT_BASE};

/// Boot-time identity table physical bases. Set once by `init` (kept for
/// diagnostics / future TTBR switches). SyncUnsafeCell over `static mut`.
use crate::klib::sync::SyncUnsafeCell;
static L0_TABLE_LO: SyncUnsafeCell<u64> = SyncUnsafeCell::new(0);
static L0_TABLE_HI: SyncUnsafeCell<u64> = SyncUnsafeCell::new(0);

/// Allocate a zeroed 4 KiB page-table page from the PMM. Returns its physical
/// address (0 on failure).
///
/// Two phases: pre-MMU every pointer is physical, so zero it directly;
/// post-MMU the active TTBR0 may not map RAM, so zero through the TTBR1 upper
/// half (`phys_to_virt`). We pick based on SCTLR_EL1.M.
fn alloc_table() -> u64 {
    let table_phys = pmm::allocate_page();
    if table_phys == 0 {
        Uart.errorln("[MMU] Failed to allocate table");
        return 0;
    }
    let sctlr = mrs!("sctlr_el1");
    let table_va = if sctlr & 1 != 0 {
        phys_to_virt(table_phys)
    } else {
        table_phys
    };
    unsafe {
        core::ptr::write_bytes(table_va as *mut u8, 0, PAGE_SIZE as usize);
    }
    table_phys
}

/// Read/write a page-table entry. `table_phys` is a physical table-page
/// address; `which_half` selects whether to deref raw (pre-MMU) or via TTBR1.
#[inline]
fn table_ptr(table_phys: u64) -> *mut u64 {
    // Routed through the upper half iff the MMU is on.
    let sctlr = mrs!("sctlr_el1");
    if sctlr & 1 != 0 {
        phys_to_virt(table_phys) as *mut u64
    } else {
        table_phys as *mut u64
    }
}

#[inline]
fn pte_read(table_phys: u64, idx: usize) -> u64 {
    unsafe { table_ptr(table_phys).add(idx).read() }
}

#[inline]
fn pte_write(table_phys: u64, idx: usize, val: u64) {
    unsafe { table_ptr(table_phys).add(idx).write(val) }
}

/// Build an L0→L1→L2 identity tree mapping the first 1 TiB of PA space with
/// 2 MiB blocks. Returns the L0 physical address. If `out_l1` is provided, the
/// first L1 table's physical address is stored there (used by the self-tests).
fn build_identity_tables(out_l1: Option<&mut u64>) -> u64 {
    let l0 = alloc_table();
    if l0 == 0 {
        return 0;
    }
    let l1 = alloc_table();
    if l1 == 0 {
        return 0;
    }
    // L0[0] -> L1 covers the first 512 GiB (RAM, device I/O, PCI ECAM).
    pte_write(l0, 0, l1 | PTE_VALID | PTE_TABLE);

    let mem_end = pmm::MEM_START + pmm::MEM_SIZE;
    for l1i in 0..512u64 {
        let l2 = alloc_table();
        if l2 == 0 {
            return 0;
        }
        pte_write(l1, l1i as usize, l2 | PTE_VALID | PTE_TABLE);

        for l2i in 0..512u64 {
            let phys_addr = l1i * SIZE_1G + l2i * SIZE_2M;
            if phys_addr == 0 {
                // Leave VA 0 unmapped so null derefs fault.
                continue;
            }
            // AttrIdx 1 (Normal) for RAM, AttrIdx 0 (Device) for everything else.
            let attr = if phys_addr >= pmm::MEM_START && phys_addr < mem_end {
                1
            } else {
                0
            };
            pte_write(
                l2,
                l2i as usize,
                phys_addr
                    | PTE_VALID
                    | PTE_BLOCK
                    | PTE_AF
                    | PTE_SH_INNER
                    | PTE_AP_RW
                    | pte_attridx(attr),
            );
        }
    }

    // L0[1] -> L1 covers 512 GiB – 1 TiB: the PCI MMIO64 window (all device).
    let l1_hi = alloc_table();
    if l1_hi == 0 {
        return 0;
    }
    pte_write(l0, 1, l1_hi | PTE_VALID | PTE_TABLE);

    for l1i in 0..512u64 {
        let l2 = alloc_table();
        if l2 == 0 {
            return 0;
        }
        pte_write(l1_hi, l1i as usize, l2 | PTE_VALID | PTE_TABLE);

        for l2i in 0..512u64 {
            let phys_addr = SIZE_512G + l1i * SIZE_1G + l2i * SIZE_2M;
            pte_write(
                l2,
                l2i as usize,
                phys_addr
                    | PTE_VALID
                    | PTE_BLOCK
                    | PTE_AF
                    | PTE_SH_INNER
                    | PTE_AP_RW
                    | pte_attridx(0),
            );
        }
    }

    if let Some(slot) = out_l1 {
        *slot = l1;
    }
    l0
}

/// Build the page tables and enable the MMU. Returns the physical address of
/// the TTBR0 first-L1 table (for the self-tests), or 0 on failure. Runs pre-MMU.
pub fn init() -> u64 {
    let uart = Uart;
    uart.println("[MMU] Initializing MMU (48 bit VAS, 4kb granule)");

    // MAIR: AttrIdx 0 = Device-nGnRnE (0x00), AttrIdx 1 = Normal WB (0xFF).
    let mair: u64 = (0x00 << 0) | (0xFF << 8);
    unsafe {
        msr!("mair_el1", mair);
    }

    // TTBR0: identity-map the low half (RAM + device + PCI).
    let mut l1_table: u64 = 0;
    let l0_lo = build_identity_tables(Some(&mut l1_table));
    if l0_lo == 0 {
        uart.errorln("[MMU] Failed to build TTBR0 tables");
        return 0;
    }
    // SAFETY (single-core): boot-time init, written once.
    unsafe {
        *L0_TABLE_LO.get() = l0_lo;
    }
    uart.println("[MMU] TTBR0 lower half tables build");

    // TTBR1: map VA 0xFFFF_0000_0000_0000+ -> PA 0x0000+. The hardware strips
    // the upper bits for TTBR1 lookups, so the same identity tree works.
    let l0_hi = build_identity_tables(None);
    if l0_hi == 0 {
        uart.errorln("[MMU] Failed to build TTBR1 tables");
        return 0;
    }
    // SAFETY (single-core): boot-time init, written once.
    unsafe {
        *L0_TABLE_HI.get() = l0_hi;
    }
    uart.println("[MMU] TTBR1 upper half tables build");

    // TCR_EL1: 48-bit VA both halves, 4 KiB granule, inner-shareable WBWA,
    // 40-bit IPS, 16-bit ASIDs sourced from TTBR0.
    let tcr: u64 = (16 << 0)        // T0SZ = 16 -> 48-bit VA (TTBR0)
        | (0b01 << 8)               // IRGN0 = WBWA
        | (0b01 << 10)              // ORGN0 = WBWA
        | (0b11 << 12)              // SH0 = inner shareable
        | (0b00 << 14)              // TG0 = 4 KiB granule
        | (16 << 16)                // T1SZ = 16 -> 48-bit VA (TTBR1)
        | (0b01 << 24)              // IRGN1 = WBWA
        | (0b01 << 26)              // ORGN1 = WBWA
        | (0b11 << 28)              // SH1 = inner shareable
        | (0b10 << 30)              // TG1 = 4 KiB granule
        | (0b010 << 32)             // IPS = 40-bit PA
        | (1 << 36); // AS = 1 -> 16-bit ASIDs (A1 = 0 -> ASID from TTBR0[63:48])

    unsafe {
        msr!("tcr_el1", tcr);
        // DDI 0487 §D5.4: full DSB ISH so table-page stores are observable to
        // the table walker before we point TTBR at them.
        core::arch::asm!("dsb ish");

        msr!("ttbr0_el1", l0_lo);
        msr!("ttbr1_el1", l0_hi);
        core::arch::asm!("dsb ish", "isb");

        core::arch::asm!("tlbi vmalle1", "dsb ish", "isb");

        // Enable MMU + caches.
        let mut sctlr = mrs!("sctlr_el1");
        sctlr |= 1 << 0; // M  = MMU enable
        sctlr |= 1 << 2; // C  = data cache
        sctlr |= 1 << 12; // I = instruction cache
        msr!("sctlr_el1", sctlr);
        core::arch::asm!("isb");
    }

    uart.println("[MMU] Enabled");
    l1_table
}

/// Walk the per-task L0→L1→L2→L3 tables, allocating intermediate table pages on
/// demand when `alloc` is true. Returns the physical address of the table page
/// holding the entry at `target_level` (2 or 3) plus the entry index, so the
/// caller can read/write it via `pte_read`/`pte_write`. Returns None on
/// allocation failure or a missing entry when `alloc` is false.
fn walk_levels(l0_table: u64, va: u64, target_level: u8, alloc: bool) -> Option<(u64, usize)> {
    let l0i = l0_index(va);
    let l1i = l1_index(va);
    let l2i = l2_index(va);
    let l3i = l3_index(va);

    // L0 -> L1
    let l0e = pte_read(l0_table, l0i);
    let l1 = if !pte_valid(l0e) {
        if !alloc {
            return None;
        }
        let l1_phys = alloc_table();
        if l1_phys == 0 {
            return None;
        }
        pte_write(l0_table, l0i, l1_phys | PTE_VALID | PTE_TABLE);
        l1_phys
    } else {
        pte_next_table(l0e)
    };

    // L1 -> L2
    let l1e = pte_read(l1, l1i);
    let l2 = if !pte_valid(l1e) {
        if !alloc {
            return None;
        }
        let l2_phys = alloc_table();
        if l2_phys == 0 {
            return None;
        }
        pte_write(l1, l1i, l2_phys | PTE_VALID | PTE_TABLE);
        l2_phys
    } else {
        pte_next_table(l1e)
    };

    if target_level == 2 {
        return Some((l2, l2i));
    }

    // L2 -> L3
    let l2e = pte_read(l2, l2i);
    let l3 = if !pte_valid(l2e) {
        if !alloc {
            return None;
        }
        let l3_phys = alloc_table();
        if l3_phys == 0 {
            return None;
        }
        pte_write(l2, l2i, l3_phys | PTE_VALID | PTE_TABLE);
        l3_phys
    } else {
        pte_next_table(l2e)
    };

    Some((l3, l3i))
}

/// Allocate an empty user L0 table (physical). L1/L2/L3 are populated lazily by
/// `map_user_range`.
pub fn create_user_tables() -> u64 {
    alloc_table()
}

/// Map `pages` contiguous 4 KiB pages [pa, pa + pages*PAGE_SIZE) at user VA `va`
/// in the user table `l0`. `flags` carries AP/UXN/PXN/ATTRIDX; VALID/AF/SH and
/// nG are added internally. `va`/`pa` must be 4 KiB-aligned.
pub fn map_user_range(l0: u64, va: u64, pa: u64, pages: u64, flags: u64) {
    for i in 0..pages {
        match walk_levels(l0, va + i * PAGE_SIZE, 3, true) {
            Some((table, idx)) => {
                // nG=1: tag with the current ASID for per-task isolation.
                let entry = ((pa + i * PAGE_SIZE) & PTE_ADDR_MASK)
                    | PTE_VALID
                    | PTE_TABLE
                    | PTE_AF
                    | PTE_SH_INNER
                    | PTE_NG
                    | flags;
                pte_write(table, idx, entry);
            }
            None => {
                Uart.errorln("[MMU] map_user_range: walk failed");
                return;
            }
        }
    }
}

/// Free every intermediate table page (L0..L3) of a user address space. The L3
/// page descriptors point at user data pages (text/stack) which are freed
/// separately by the scheduler.
pub fn free_user_tables(l0_phys: u64) {
    for i in 0..512usize {
        let l0e = pte_read(l0_phys, i);
        if !pte_valid(l0e) {
            continue;
        }
        let l1_phys = pte_next_table(l0e);

        for j in 0..512usize {
            let l1e = pte_read(l1_phys, j);
            if !pte_valid(l1e) {
                continue;
            }
            // bit[1]==1 -> table descriptor; ==0 -> 1 GiB block (no L2).
            if l1e & PTE_TABLE == 0 {
                continue;
            }
            let l2_phys = pte_next_table(l1e);

            for k in 0..512usize {
                let l2e = pte_read(l2_phys, k);
                if !pte_valid(l2e) {
                    continue;
                }
                // bit[1]==1 -> L3 table; ==0 -> legacy 2 MiB block.
                if l2e & PTE_TABLE == 0 {
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

// --- Self-tests (run post-MMU, before any per-task TTBR0 takes over) ----------

fn print_result(name: &str, pass: bool) {
    kprintln!("[MMU TEST] {}: {}", name, if pass { "PASS" } else { "FAIL" });
}

fn test_mmu_enabled() -> bool {
    mrs!("sctlr_el1") & 1 != 0
}

fn test_identity_mapping() -> bool {
    let page = pmm::allocate_page();
    if page == 0 {
        Uart.errorln("[MMU TEST] Failed to allocate page for identity mapping test");
        return false;
    }
    let ptr = page as *mut u64;
    unsafe {
        ptr.write_volatile(0xAABBCCDD);
        let pass = ptr.read_volatile() == 0xAABBCCDD;
        pmm::free_page(page);
        pass
    }
}

/// L2 remap test: rewrite an L2 PTE in the boot identity table to point at a
/// freshly-allocated 2 MiB region, write through the lower-half VA, and verify
/// it lands at the new PA via the TTBR1 upper half. Requires TTBR0 to still be
/// the boot identity table (true right after `init`, before any user task).
fn test_remap_l2(l1_table_phys: u64) -> bool {
    kprintln!("[MMU TEST] L2 remap test");

    let l1_idx = 1usize; // 1 GiB region — safely past the kernel
    let l2_idx = 10usize; // arbitrary 2 MiB chunk

    let l1e = pte_read(l1_table_phys, l1_idx);
    let l2_phys = pte_next_table(l1e);
    let old = pte_read(l2_phys, l2_idx);

    // Get a 2 MiB-aligned chunk (ask 4 MiB, use the aligned half).
    let alloc_phys = pmm::allocate_pages(1024); // 4 MiB
    if alloc_phys == 0 {
        Uart.errorln("[MMU TEST] L2 remap: allocate_pages failed");
        return false;
    }
    let new_phys = (alloc_phys + (SIZE_2M - 1)) & !(SIZE_2M - 1);
    let pre_pad_pages = (new_phys - alloc_phys) / PAGE_SIZE;
    let post_pad_pages = 1024 - pre_pad_pages - 512;

    pte_write(
        l2_phys,
        l2_idx,
        new_phys | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER | PTE_AP_RW | pte_attridx(1),
    );
    unsafe {
        core::arch::asm!("tlbi vmalle1", "dsb ish", "isb");
    }

    let va = (l1_idx as u64) * SIZE_1G + (l2_idx as u64) * SIZE_2M;
    let pass;
    unsafe {
        (va as *mut u64).write_volatile(0xCAFEBABE);
        pass = (phys_to_virt(new_phys) as *mut u64).read_volatile() == 0xCAFEBABE;
    }

    // Restore original mapping, flush, return the pages.
    pte_write(l2_phys, l2_idx, old);
    unsafe {
        core::arch::asm!("tlbi vmalle1", "dsb ish", "isb");
    }

    if pre_pad_pages != 0 {
        pmm::free_pages(alloc_phys, pre_pad_pages);
    }
    pmm::free_pages(new_phys, 512);
    if post_pad_pages != 0 {
        pmm::free_pages(new_phys + 512 * PAGE_SIZE, post_pad_pages);
    }
    pass
}

fn test_ttbr1_upper_half() -> bool {
    kprintln!("[MMU TEST] TTBR1 upper half access test");

    let pa = pmm::allocate_page();
    if pa == 0 {
        Uart.errorln("[MMU TEST] Failed to allocate page for TTBR1 test");
        return false;
    }
    let pass;
    unsafe {
        let lo = pa as *mut u64;
        lo.write_volatile(0xABCDEFAD);
        core::arch::asm!("dsb ish");

        let hi = phys_to_virt(pa) as *mut u64;
        kprintln!("[MMU TEST] lo_ptr={:#x} hi_ptr={:#x}", pa, phys_to_virt(pa));

        let mut p = hi.read_volatile() == 0xABCDEFAD;

        hi.write_volatile(0xABBCCCDD);
        core::arch::asm!("dsb ish");
        p &= lo.read_volatile() == 0xABBCCCDD;
        pass = p;
    }
    pmm::free_page(pa);
    pass
}

/// Run the MMU self-tests. `l1_table_phys` is the value returned by `init`.
pub fn run_tests(l1_table_phys: u64) {
    print_result("MMU Enabled", test_mmu_enabled());
    print_result("Identity Mapping", test_identity_mapping());
    print_result("L2 Remap", test_remap_l2(l1_table_phys));
    print_result("TTBR1 Upper Half", test_ttbr1_upper_half());
}
