//! Shared memory-management constants: page-table entry (PTE) format, virtual
//! address layout, and TTBR packing. Defined once here and consumed by the
//! MMU, scheduler, and syscall/ELF paths so the page-table ABI stays
//! consistent (see docs/cref/00-PORT-PLAN.md §2.3/§2.4).

// --- Granule / sizes ---------------------------------------------------------

pub const PAGE_SIZE: u64 = 4096;
pub const PAGE_SHIFT: u32 = 12;

pub const SIZE_512G: u64 = 0x80_0000_0000;
pub const SIZE_1G: u64 = 0x4000_0000;
pub const SIZE_2M: u64 = 0x20_0000;

// --- Higher-half mapping -----------------------------------------------------

/// Offset between a physical address and its kernel (TTBR1) virtual mapping.
pub const KERNEL_VA_OFFSET: u64 = 0xFFFF_0000_0000_0000;

#[inline]
pub const fn phys_to_virt(pa: u64) -> u64 {
    pa + KERNEL_VA_OFFSET
}

#[inline]
pub const fn virt_to_phys(va: u64) -> u64 {
    va - KERNEL_VA_OFFSET
}

// --- Descriptor type bits [1:0] ----------------------------------------------
// 00/10 -> invalid, 01 -> block, 11 -> table (L0/L1/L2) or page (L3)

pub const PTE_VALID: u64 = 1 << 0;
pub const PTE_TABLE: u64 = 1 << 1;
pub const PTE_BLOCK: u64 = 0 << 1;

// --- Lower attributes --------------------------------------------------------

/// Access flag — the CPU raises an access fault on first use if AF==0.
pub const PTE_AF: u64 = 1 << 10;
/// Inner-shareable.
pub const PTE_SH_INNER: u64 = 3 << 8;

// Access permissions AP[2:1] (block/page descriptor), stage-1 EL1&0 regime:
//   00 -> EL1 RW, EL0 none      01 -> EL1 RW, EL0 RW
//   10 -> EL1 RO, EL0 none      11 -> EL1 RO, EL0 RO
pub const PTE_AP_RW: u64 = 0 << 6;
pub const PTE_AP_RW_EL0: u64 = 1 << 6;
pub const PTE_AP_RO: u64 = 2 << 6;
pub const PTE_AP_RO_EL0: u64 = 3 << 6;

/// Memory type index into MAIR_EL1.
#[inline]
pub const fn pte_attridx(idx: u64) -> u64 {
    idx << 2
}

/// Non-global: TLB entries are tagged with the current ASID (TTBR0[63:48]).
/// User mappings set nG=1 for per-task isolation; kernel mappings leave nG=0
/// so they are global across every ASID.
pub const PTE_NG: u64 = 1 << 11;

// --- Upper attributes --------------------------------------------------------

/// Privileged execute-never (kernel cannot execute).
pub const PTE_PXN: u64 = 1 << 53;
/// Unprivileged execute-never (user cannot execute).
pub const PTE_UXN: u64 = 1 << 54;

/// Output-address mask for a 4 KiB granule, 48-bit OA: bits [47:12].
pub const PTE_ADDR_MASK: u64 = 0x0000_FFFF_FFFF_F000;

#[inline]
pub const fn pte_valid(entry: u64) -> bool {
    entry & PTE_VALID != 0
}

/// Physical address of the next-level table referenced by `entry`.
#[inline]
pub const fn pte_next_table(entry: u64) -> u64 {
    entry & PTE_ADDR_MASK
}

// --- VA index extraction (4 KiB granule, 4-level) ----------------------------

#[inline]
pub const fn l0_index(va: u64) -> usize {
    ((va >> 39) & 0x1FF) as usize
}
#[inline]
pub const fn l1_index(va: u64) -> usize {
    ((va >> 30) & 0x1FF) as usize
}
#[inline]
pub const fn l2_index(va: u64) -> usize {
    ((va >> 21) & 0x1FF) as usize
}
#[inline]
pub const fn l3_index(va: u64) -> usize {
    ((va >> 12) & 0x1FF) as usize
}

// --- TTBR packing (ASID in TTBR0[63:48] when TCR.AS=1, A1=0) -----------------

pub const TTBR_ASID_SHIFT: u64 = 48;
pub const TTBR_BADDR_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;

#[inline]
pub const fn ttbr_pack(baddr: u64, asid: u16) -> u64 {
    (baddr & TTBR_BADDR_MASK) | ((asid as u64) << TTBR_ASID_SHIFT)
}

#[inline]
pub const fn ttbr_baddr(ttbr: u64) -> u64 {
    ttbr & TTBR_BADDR_MASK
}

#[inline]
pub const fn ttbr_asid(ttbr: u64) -> u16 {
    (ttbr >> TTBR_ASID_SHIFT) as u16
}

// --- User-space address layout (TTBR0) ---------------------------------------

pub const USER_TEXT_BASE: u64 = 0x0040_0000; // 4 MiB — user code
pub const USER_STACK_TOP: u64 = 0x0080_0000; // 8 MiB — top of user stack
pub const USER_STACK_PAGES: u64 = 4; // initial 16 KiB user stack
