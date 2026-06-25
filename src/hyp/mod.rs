//! Fermi EL2 Type-1 hypervisor — Milestone 1: bring-up.
//!
//! Boot order (see `arch/boot.S`): QEMU enters the image at EL2 when the machine
//! is started with `virtualization=on`. `boot.S` detects EL2, calls
//! [`hyp_init`] to configure the hypervisor, then `eret`s down to EL1 where the
//! existing Fermi kernel continues to run — now as a stage-2-translated guest.
//! If the image is entered at EL1 directly (virtualization off), the EL2 path is
//! skipped entirely and the kernel boots as a plain EL1 OS.
//!
//! Everything in [`hyp_init`] runs at EL2 with the **EL2 MMU off**. All symbol
//! references are PC-relative (`adrp`/`add`), so taking the address of a static
//! while the PC is physical yields its *physical* address — exactly what
//! `VTTBR_EL2` / `VBAR_EL2` need. This mirrors how `early_init` runs before the
//! stage-1 MMU is enabled. Consequently this module must avoid anything that
//! materializes an absolute upper-half VA (no `kprintln!`/`core::fmt`); it logs
//! through a self-contained PL011 writer and formats hex by hand.

use crate::mm::consts::{SIZE_1G, SIZE_512G};
use crate::mm::pmm;
use crate::mrs;
use crate::msr;
use core::arch::global_asm;
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, Ordering};

// EL2 exception vector table + el2_common save/restore trampoline.
global_asm!(include_str!("vector_el2.S"));

// --- Stage-2 (VMSAv8-64) descriptor bits ------------------------------------
// These differ from stage-1: no MAIR indirection (MemAttr[5:2] encodes the type
// directly), access permission is S2AP[7:6], execute permission is XN[54:53]
// (left 0 = executable).
const S2_VALID: u64 = 1 << 0;
const S2_TABLE: u64 = 3 << 0; // bits[1:0]=11: table (L0/L1) or page (L3)
const S2_AF: u64 = 1 << 10; // Access flag
const S2_SH_INNER: u64 = 3 << 8; // Inner shareable
const S2_AP_RW: u64 = 3 << 6; // S2AP = read/write at EL0 & EL1
const S2_MEM_NORMAL: u64 = 0xF << 2; // MemAttr = Normal Inner+Outer WB
const S2_MEM_DEVICE: u64 = 0x0 << 2; // MemAttr = Device-nGnRnE

// --- HCR_EL2 bits ------------------------------------------------------------
const HCR_VM: u64 = 1 << 0; // Enable stage-2 translation for EL1&0
const HCR_RW: u64 = 1 << 31; // EL1 execution state is AArch64

// --- CNTHCTL_EL2 bits (non-VHE): let EL1/EL0 reach the physical counter/timer
const CNTHCTL_EL1PCTEN: u64 = 1 << 0;
const CNTHCTL_EL1PCEN: u64 = 1 << 1;

// --- Exception-class values in ESR_EL2[31:26] --------------------------------
const ESR_EC_SHIFT: u64 = 26;
const ESR_EC_MASK: u64 = 0x3F;
const EC_HVC64: u64 = 0x16; // HVC instruction execution in AArch64 state
const EC_DABT_LOWER: u64 = 0x24; // Data abort from a lower EL (stage-2 fault)
const EC_IABT_LOWER: u64 = 0x20; // Instruction abort from a lower EL

// --- Hypervisor-private storage (.hyp_tables) --------------------------------
// Placed (by linker.ld) AFTER __bss_end so the EL1 guest's zero_bss() never
// wipes the live stage-2 tables, and BEFORE __kernel_end so the guest PMM
// reserves these pages instead of handing them out. The section is NOLOAD, so
// the zero initializers below cost no file space and are NOT present at runtime
// — every table is filled explicitly by `hyp_build_stage2` before use.

use crate::klib::sync::SyncUnsafeCell;

/// A 4 KiB-aligned 512-entry page-table page. Stage-2 table bases must be
/// page-aligned for VTTBR_EL2 / the table walker.
#[repr(C, align(4096))]
struct PageTable([u64; 512]);

/// Dedicated EL2 trap stack. `boot.S` repoints SP_EL2 to its top before the
/// `eret`, so guest→EL2 traps never clobber the EL1 kernel stack (SP_EL1).
/// Exported under the exact symbol `el2_stack` that `boot.S` references.
#[repr(C, align(16))]
struct El2Stack([u8; 8192]);

#[export_name = "el2_stack"]
#[link_section = ".hyp_tables"]
static EL2_STACK: El2Stack = El2Stack([0; 8192]);

// Stage-2 page tables. 4 KiB granule, 48-bit IPA input (T0SZ=16, start at L0),
// 40-bit PA output. 1 GiB blocks at L1, so the whole 0..1 TiB IPA space is
// described by exactly three tables — no L2/L3 needed at this milestone.
#[link_section = ".hyp_tables"]
static S2_L0: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static S2_L1_LOW: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512])); // 0..512 GiB
#[link_section = ".hyp_tables"]
static S2_L1_HIGH: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512])); // 512G..1 TiB

/// One-shot guard so an unexpected lower-EL abort dumps context once and then
/// parks, instead of an endless re-fault spam loop. Lives in `.bss` (zeroed by
/// `zero_bss` on the EL1 path, which always runs before any guest can fault).
static DUMPED: AtomicBool = AtomicBool::new(false);

// --- self-contained PL011 output (no driver state, safe pre-uart_init) -------
// EL2 runs MMU-off; the PL011 is reached at its physical base directly, with no
// dependency on the mmio module's VA offset or on `uart::init` having run.

const HYP_UART_DR: usize = 0x0900_0000;
const HYP_UART_FR: usize = 0x0900_0018;
const HYP_UART_FR_TXFF: u32 = 1 << 5;

fn hyp_putc(c: u8) {
    // SAFETY: fixed PL011 MMIO; physical address valid at EL2 with the MMU off.
    unsafe {
        while read_volatile(HYP_UART_FR as *const u32) & HYP_UART_FR_TXFF != 0 {}
        write_volatile(HYP_UART_DR as *mut u32, c as u32);
    }
}

fn hyp_puts(s: &str) {
    for b in s.bytes() {
        if b == b'\n' {
            hyp_putc(b'\r');
        }
        hyp_putc(b);
    }
}

fn hyp_puthex(v: u64) {
    hyp_puts("0x");
    let mut shift: i32 = 60;
    while shift >= 0 {
        let nib = ((v >> shift) & 0xF) as u8;
        hyp_putc(if nib < 10 {
            b'0' + nib
        } else {
            b'a' + nib - 10
        });
        shift -= 4;
    }
}

/// Physical address of a `.hyp_tables` static (PC is physical here).
#[inline]
fn phys_of<T>(p: *const T) -> u64 {
    p as usize as u64
}

/// Build the IPA == PA identity map with 1 GiB blocks. RAM [MEM_START,
/// MEM_START+MEM_SIZE) is mapped Normal WB; everything else (GIC, UART, PCI
/// ECAM/MMIO) is mapped Device-nGnRnE.
///
/// Pointers are physical here (PC-relative, MMU off), which is exactly what the
/// descriptors and VTTBR_EL2 must contain.
fn hyp_build_stage2() {
    let mem_start = pmm::MEM_START;
    let mem_end = pmm::MEM_START + pmm::MEM_SIZE;

    let l0 = S2_L0.get();
    let l1_low = S2_L1_LOW.get();
    let l1_high = S2_L1_HIGH.get();
    let l1_low_phys = phys_of(l1_low);
    let l1_high_phys = phys_of(l1_high);

    // SAFETY (single-core, pre-guest): these tables are hypervisor-private and
    // touched only here before any guest runs; raw access is sound.
    unsafe {
        // L0: only the first two 512 GiB regions are populated.
        for e in (*l0).0.iter_mut() {
            *e = 0;
        }
        (*l0).0[0] = l1_low_phys | S2_TABLE | S2_VALID;
        (*l0).0[1] = l1_high_phys | S2_TABLE | S2_VALID;

        // L1 low: 512 x 1 GiB blocks covering IPA 0 .. 512 GiB.
        for i in 0..512u64 {
            let pa = i * SIZE_1G;
            let (mem, sh) = if pa >= mem_start && pa < mem_end {
                (S2_MEM_NORMAL, S2_SH_INNER)
            } else {
                (S2_MEM_DEVICE, 0)
            };
            (*l1_low).0[i as usize] = pa | S2_VALID | S2_AF | sh | S2_AP_RW | mem;
        }

        // L1 high: 512 x 1 GiB blocks covering IPA 512 GiB .. 1 TiB — all
        // device (the PCI MMIO64 window).
        for i in 0..512u64 {
            let pa = SIZE_512G + i * SIZE_1G;
            (*l1_high).0[i as usize] = pa | S2_VALID | S2_AF | S2_AP_RW | S2_MEM_DEVICE;
        }

        core::arch::asm!("dsb ish");
    }
}

/// Configure EL2 and stage-2, install the EL2 vector table. Called once from
/// `boot.S` while still at EL2, MMU off. `boot.S` performs the `eret` to EL1.
#[no_mangle]
pub extern "C" fn hyp_init() {
    // Permit FP/SIMD at EL2 (clear CPTR_EL2.TFP) so any auto-vectorized codegen
    // in this module can't take an FP trap. We keep the hot paths scalar, so
    // this is purely defensive and does not disturb guest FP state.
    // SAFETY: configures the current (EL2) CPU; no memory effects.
    unsafe {
        let mut cptr = mrs!("cptr_el2");
        cptr &= !(1u64 << 10); // TFP
        msr!("cptr_el2", cptr);
        core::arch::asm!("isb");
    }

    hyp_puts("\n[HYP] Fermi hypervisor online at EL2\n");

    // Sanity: confirm we really are at EL2.
    let el = (mrs!("CurrentEL") >> 2) & 0x3;
    hyp_puts("[HYP] CurrentEL = ");
    hyp_puthex(el);
    hyp_puts("\n");

    // Generic timer: zero the virtual offset and let EL1/EL0 use the physical
    // counter and timer registers directly (Fermi drives the timer from EL1).
    // SAFETY: EL2 timer-control registers; no memory effects.
    unsafe {
        msr!("cntvoff_el2", 0u64);
        let mut cnthctl = mrs!("cnthctl_el2");
        cnthctl |= CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN;
        msr!("cnthctl_el2", cnthctl);
    }

    // Stage-2 translation tables.
    hyp_build_stage2();

    // VTCR_EL2: 4 KiB granule, 48-bit IPA (T0SZ=16, SL0=2 => start at L0),
    // 40-bit PA output (PS=2 => 1 TiB), inner-shareable WB walks.
    let vtcr: u64 = (16 << 0)    // T0SZ = 16 -> 48-bit IPA
        | (2 << 6)               // SL0  = 2  -> start at level 0
        | (1 << 8)               // IRGN0 = WB/WA
        | (1 << 10)              // ORGN0 = WB/WA
        | (3 << 12)              // SH0   = inner shareable
        | (0 << 14)              // TG0   = 4 KiB granule
        | (2 << 16); // PS = 40-bit (1 TiB) PA
    let s2_l0_phys = phys_of(S2_L0.get());
    let vbar = phys_of(core::ptr::addr_of!(el2_vector_table));

    // SAFETY: programs the EL2 stage-2 / vector base, then enables stage-2.
    // Address operands are physical (MMU off), as the hardware requires.
    unsafe {
        msr!("vtcr_el2", vtcr);
        // VTTBR_EL2: physical base of the stage-2 L0 table, VMID = 0.
        msr!("vttbr_el2", s2_l0_phys);
        // Install the EL2 vector table (physical address; VBAR_EL2 wants PA).
        msr!("vbar_el2", vbar);
        core::arch::asm!("isb");

        // Enable stage-2 and pin EL1 to AArch64. From here the EL1 guest's
        // physical accesses are IPA->PA translated by the tables above.
        msr!("hcr_el2", HCR_RW | HCR_VM);
        core::arch::asm!("isb");
    }

    hyp_puts("[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

// --- traps -------------------------------------------------------------------

extern "C" {
    /// EL2 vector table base (defined in `vector_el2.S`).
    static el2_vector_table: u8;
}

/// Minimal trap frame pushed by the EL2 vector stubs (x0..x30). The stub
/// reserves 256 bytes for 16-byte SP alignment; only the first 248 (31 * 8) are
/// the saved GP registers.
#[repr(C)]
pub struct El2Frame {
    pub x: [u64; 31],
}

fn ec_name(ec: u64) -> &'static str {
    match ec {
        EC_HVC64 => "HVC (hypercall)",
        EC_DABT_LOWER => "data abort (stage-2)",
        EC_IABT_LOWER => "instruction abort (stage-2)",
        _ => "other",
    }
}

/// C dispatcher for EL2 exceptions. `index` is the vector slot (0..15); 8 = sync
/// from a lower EL (AArch64), which is where guest HVC/aborts land.
#[no_mangle]
pub extern "C" fn el2_dispatch(index: u64, _frame: *mut El2Frame) {
    let esr = mrs!("esr_el2");
    let elr = mrs!("elr_el2");
    let ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    hyp_puts("\n[HYP] *** EL2 trap *** vector=");
    hyp_puthex(index);
    hyp_puts(" EC=");
    hyp_puthex(ec);
    hyp_puts(" (");
    hyp_puts(ec_name(ec));
    hyp_puts(")\n      ESR_EL2=");
    hyp_puthex(esr);
    hyp_puts(" ELR_EL2=");
    hyp_puthex(elr);
    hyp_puts("\n");

    if ec == EC_HVC64 {
        // HVC #imm: the 16-bit immediate is in ESR_EL2[15:0]. ELR_EL2 already
        // points to the instruction *after* the HVC, so a plain eret resumes
        // the guest correctly. This is the Milestone-1 proof that guest->EL2
        // world transitions work.
        hyp_puts("      hypercall imm=");
        hyp_puthex(esr & 0xFFFF);
        hyp_puts(", returning to guest\n\n");
        return;
    }

    // Any other trap at this milestone is unexpected. For a lower-EL abort, dump
    // the full stage-2 context ONCE then park the CPU, so the log is readable
    // instead of an endless re-fault spam loop.
    if !DUMPED.swap(true, Ordering::Relaxed) {
        let far = mrs!("far_el2");
        let hpfar = mrs!("hpfar_el2");
        let ipa = (hpfar >> 4) << 12; // HPFAR[43:4] = IPA[51:12]
        hyp_puts("      FAR_EL2=");
        hyp_puthex(far);
        hyp_puts(" HPFAR_EL2=");
        hyp_puthex(hpfar);
        hyp_puts("\n      faulting IPA=");
        hyp_puthex(ipa);
        hyp_puts("\n      VTTBR_EL2=");
        hyp_puthex(mrs!("vttbr_el2"));
        hyp_puts(" VTCR_EL2=");
        hyp_puthex(mrs!("vtcr_el2"));
        hyp_puts("\n      HCR_EL2=");
        hyp_puthex(mrs!("hcr_el2"));
        // SAFETY (single-core): inspecting hypervisor-private tables for the
        // crash dump; no concurrent writer once we have faulted.
        unsafe {
            hyp_puts("\n      &s2_l0=");
            hyp_puthex(phys_of(S2_L0.get()));
            hyp_puts(" s2_l0[0]=");
            hyp_puthex((*S2_L0.get()).0[0]);
            hyp_puts("\n      &s2_l1_low=");
            hyp_puthex(phys_of(S2_L1_LOW.get()));
            hyp_puts(" s2_l1_low[1]=");
            hyp_puthex((*S2_L1_LOW.get()).0[1]);
        }
        hyp_puts("\n[HYP] parking CPU for inspection.\n");
        loop {
            unsafe { core::arch::asm!("wfi") };
        }
    }
    hyp_puts("[HYP] unhandled EL2 exception (continuing)\n\n");
}
