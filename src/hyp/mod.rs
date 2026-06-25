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

pub mod hypercall;

use crate::mm::consts::{PAGE_SIZE, SIZE_1G, SIZE_2M, SIZE_512G};
use crate::mm::pmm;
use crate::mrs;
use crate::msr;
use core::arch::global_asm;
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, Ordering};

// EL2 exception vector table + el2_common save/restore trampoline.
global_asm!(include_str!("vector_el2.S"));
// Tiny second guest payload (M5a), copied into guest 1's RAM slice.
global_asm!(include_str!("guest1.S"));

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
const HCR_IMO: u64 = 1 << 4; // Route physical IRQ to EL2 + enable vIRQ
const HCR_TID3: u64 = 1 << 18; // Trap ID group 3 (ID_AA64*) reads to EL2
const HCR_RW: u64 = 1 << 31; // EL1 execution state is AArch64

// --- GICv3 EL2 control bits (System Register / virtual CPU interface) --------
const ICC_SRE_SRE: u64 = 1 << 0; // System Register interface enable
const ICC_SRE_ENABLE: u64 = 1 << 3; // allow lower-EL ICC_SRE access (no trap)
const ICC_CTLR_EOIMODE: u64 = 1 << 1; // EOIR1 = priority drop only (no deact)
const ICH_HCR_EN: u64 = 1 << 0; // enable the virtual CPU interface

// ICH_LR<n>_EL2 list-register fields:
const ICH_LR_GROUP1: u64 = 1 << 60;
const ICH_LR_HW: u64 = 1 << 61;
const ICH_LR_STATE_PENDING: u64 = 1 << 62; // State[63:62] = 0b01
const ICH_LR_PINTID_SHIFT: u64 = 32;
const ICH_LR_PRIO_SHIFT: u64 = 48;

// --- CNTHCTL_EL2 bits (non-VHE): let EL1/EL0 reach the physical counter/timer
const CNTHCTL_EL1PCTEN: u64 = 1 << 0;
const CNTHCTL_EL1PCEN: u64 = 1 << 1;

// --- Exception-class values in ESR_EL2[31:26] --------------------------------
const ESR_EC_SHIFT: u64 = 26;
const ESR_EC_MASK: u64 = 0x3F;
const EC_SYSREG: u64 = 0x18; // Trapped MSR/MRS/system instruction (AArch64)
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
// described by three tables; two more split the one 1 GiB block holding
// hypervisor RAM down to 4 KiB so we can punch an isolation hole (M3).
#[link_section = ".hyp_tables"]
static S2_L0: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static S2_L1_LOW: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512])); // 0..512 GiB
#[link_section = ".hyp_tables"]
static S2_L1_HIGH: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512])); // 512G..1 TiB

// Split tables for the 1 GiB region containing hypervisor-private RAM: L2 splits
// that 1 GiB into 512 x 2 MiB; L3 splits the one 2 MiB block holding
// [__hyp_start, __hyp_end) into 512 x 4 KiB, with the hyp pages left invalid.
#[link_section = ".hyp_tables"]
static S2_L2_SPLIT: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static S2_L3_SPLIT: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));

extern "C" {
    /// Hypervisor-private region bounds (linker symbols, see `linker.ld`). Their
    /// addresses taken pre-MMU/at EL2 are physical == guest IPA (identity map).
    static __hyp_start: u8;
    static __hyp_end: u8;
}

/// Physical/IPA base of `[__hyp_start, __hyp_end)`.
#[inline]
fn hyp_region() -> (u64, u64) {
    (
        core::ptr::addr_of!(__hyp_start) as u64,
        core::ptr::addr_of!(__hyp_end) as u64,
    )
}

// vCPU lifecycle state.
const VCPU_UNUSED: u32 = 0;
const VCPU_READY: u32 = 1;
const VCPU_RUNNING: u32 = 2;

const NUM_VCPUS: usize = 2;

/// Per-vCPU control block: everything needed to suspend a guest at EL2 and later
/// resume it. Lives in hypervisor-private memory (`.hyp_tables`) so guests can
/// neither see nor zero it. The GP regs come from / go to the EL2 trap frame;
/// PC/PSTATE are ELR_EL2/SPSR_EL2; the EL1 system registers are saved from /
/// restored to the live CPU on each world switch. The physical timer (CNTP_*)
/// is deliberately NOT context-switched here — it stays owned by the primary
/// guest, and its IRQs are injected as they arrive.
#[repr(C)]
#[derive(Clone, Copy)]
struct Vcpu {
    id: u64,
    state: u32,

    // Statistics.
    hvc_count: u64,
    sysreg_traps: u64,
    abort_count: u64,
    virq_injected: u64,

    // Saved execution state.
    regs: [u64; 31], // x0..x30
    pc: u64,         // resume PC (ELR_EL2)
    pstate: u64,     // resume PSTATE (SPSR_EL2)
    vttbr: u64,      // stage-2 base | (VMID << 48)

    // Saved EL1 system-register context.
    sp_el1: u64,
    elr_el1: u64,
    spsr_el1: u64,
    sctlr_el1: u64,
    cpacr_el1: u64,
    ttbr0_el1: u64,
    ttbr1_el1: u64,
    tcr_el1: u64,
    mair_el1: u64,
    amair_el1: u64,
    vbar_el1: u64,
    contextidr_el1: u64,
    tpidr_el1: u64,
    tpidrro_el0: u64,
    tpidr_el0: u64,
    esr_el1: u64,
    far_el1: u64,
    par_el1: u64,
}

impl Vcpu {
    const fn zeroed() -> Self {
        Vcpu {
            id: 0,
            state: VCPU_UNUSED,
            hvc_count: 0,
            sysreg_traps: 0,
            abort_count: 0,
            virq_injected: 0,
            regs: [0; 31],
            pc: 0,
            pstate: 0,
            vttbr: 0,
            sp_el1: 0,
            elr_el1: 0,
            spsr_el1: 0,
            sctlr_el1: 0,
            cpacr_el1: 0,
            ttbr0_el1: 0,
            ttbr1_el1: 0,
            tcr_el1: 0,
            mair_el1: 0,
            amair_el1: 0,
            vbar_el1: 0,
            contextidr_el1: 0,
            tpidr_el1: 0,
            tpidrro_el0: 0,
            tpidr_el0: 0,
            esr_el1: 0,
            far_el1: 0,
            par_el1: 0,
        }
    }
}

/// The vCPUs and the index of the one currently running. In `.hyp_tables`
/// (NOLOAD); initialised explicitly in `hyp_init`.
#[link_section = ".hyp_tables"]
static VCPUS: SyncUnsafeCell<[Vcpu; NUM_VCPUS]> = SyncUnsafeCell::new([Vcpu::zeroed(); NUM_VCPUS]);
#[link_section = ".hyp_tables"]
static CURRENT_VCPU: SyncUnsafeCell<usize> = SyncUnsafeCell::new(0);

// --- Guest 1: a tiny second guest with its own stage-2 + RAM slice -----------
// The hole-punch in hyp_build_stage2 keeps this region invisible to guest 0.
// Guest 1 runs with its stage-1 MMU off, so it sees IPA == PA over just this
// slice; every other IPA is unmapped, sandboxing it.
const GUEST1_RAM_SIZE: usize = 16 * 1024;
#[link_section = ".hyp_tables"]
static G1_L0: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static G1_L1: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static G1_L2: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));
#[link_section = ".hyp_tables"]
static G1_L3: SyncUnsafeCell<PageTable> = SyncUnsafeCell::new(PageTable([0; 512]));

/// Guest 1's RAM slice. 4 KiB-aligned so its stage-2 maps it cleanly.
#[repr(C, align(4096))]
struct Guest1Ram([u8; GUEST1_RAM_SIZE]);
#[link_section = ".hyp_tables"]
static GUEST1_RAM: SyncUnsafeCell<Guest1Ram> = SyncUnsafeCell::new(Guest1Ram([0; GUEST1_RAM_SIZE]));

extern "C" {
    static guest1_payload: u8;
    static guest1_payload_end: u8;
}

/// Accessor for the running vCPU (mutable pointer). Single-core: only ever used
/// from EL2 trap context, which cannot reenter.
#[inline]
fn cur_vcpu() -> *mut Vcpu {
    // SAFETY (single-core): EL2 trap context has exclusive access.
    unsafe {
        let idx = *CURRENT_VCPU.get();
        &mut (*VCPUS.get())[idx]
    }
}

/// One-shot guard so an unexpected lower-EL abort dumps context once and then
/// parks, instead of an endless re-fault spam loop. Lives in `.bss` (zeroed by
/// `zero_bss` on the EL1 path, which always runs before any guest can fault).
static DUMPED: AtomicBool = AtomicBool::new(false);

/// Whether the kernel was launched at EL2 (a hypervisor is active beneath the
/// EL1 guest). `boot.S` carries the EL2-vs-EL1 entry decision in a callee-saved
/// register across the eret and zero_bss, and `early_init` records it here via
/// [`set_booted_via_el2`] — so this is set *after* zero_bss and survives. This
/// gates guest-side `hvc_call`: on a bare EL1 boot (the host-QEMU CI path, no
/// `virtualization=on`) an `HVC` would trap as UNDEFINED, so callers must check.
static BOOTED_VIA_EL2: AtomicBool = AtomicBool::new(false);

/// Record whether boot entered at EL2. Called once from `early_init` with the
/// indicator `boot.S` threaded through from the entry-EL check.
pub fn set_booted_via_el2(yes: bool) {
    BOOTED_VIA_EL2.store(yes, Ordering::Relaxed);
}

/// Whether the kernel was launched at EL2 (a hypervisor is active beneath the
/// EL1 guest). Guards guest-side `hvc_call` use.
pub fn booted_via_el2() -> bool {
    BOOTED_VIA_EL2.load(Ordering::Relaxed)
}

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

        // --- Isolation: deny the guest any stage-2 mapping of hypervisor RAM ---
        //
        // The hypervisor's private region [__hyp_start, __hyp_end) lives inside
        // one 1 GiB block of RAM. Split that block: 1 GiB -> 512 x 2 MiB
        // (s2_l2_split), and the single 2 MiB block that contains the region ->
        // 512 x 4 KiB (s2_l3_split). Then mark the 4 KiB pages covering the hyp
        // region invalid. The hardware table walker reaches these split tables
        // via VTTBR physical addresses, so unmapping them from the guest IPA
        // view is safe.
        let (hs, he) = hyp_region();
        let gb_idx = hs / SIZE_1G; // which s2_l1_low entry
        let region_base = gb_idx * SIZE_1G;
        let mb_idx = (hs - region_base) / SIZE_2M; // 2 MiB block within region
        let mb_base = region_base + mb_idx * SIZE_2M;

        let l2_split = S2_L2_SPLIT.get();
        let l3_split = S2_L3_SPLIT.get();

        // L2 split: identity 2 MiB blocks for the whole 1 GiB RAM region.
        for b in 0..512u64 {
            let pa = region_base + b * SIZE_2M;
            (*l2_split).0[b as usize] =
                pa | S2_VALID | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
        }

        // L3 split: identity 4 KiB pages for the 2 MiB block holding the hyp
        // region, with the hyp pages left invalid (unmapped). Page descriptors
        // at L3 use bits[1:0]=11 (S2_TABLE encoding).
        for p in 0..512u64 {
            let pa = mb_base + p * PAGE_SIZE;
            (*l3_split).0[p as usize] = if pa >= (hs & !(PAGE_SIZE - 1)) && pa < he {
                0 // hole: hypervisor-private, guest has no access
            } else {
                pa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL
            };
        }

        // Splice the split tables in, replacing the original 1 GiB block.
        (*l2_split).0[mb_idx as usize] = phys_of(l3_split) | S2_TABLE | S2_VALID;
        (*l1_low).0[gb_idx as usize] = phys_of(l2_split) | S2_TABLE | S2_VALID;

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

    // vCPU table starts empty; populated near the end of hyp_init once the
    // stage-2 tables and guest 1 payload are in place (NOLOAD => not zeroed).
    // SAFETY (single-core, pre-guest): exclusive access during boot.
    unsafe {
        *VCPUS.get() = [Vcpu::zeroed(); NUM_VCPUS];
        *CURRENT_VCPU.get() = 0;
    }

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

    // Stage-2 translation tables (with the hypervisor's own RAM unmapped).
    hyp_build_stage2();
    let (hs, he) = hyp_region();
    hyp_puts("[HYP] isolated hyp region [");
    hyp_puthex(hs);
    hyp_puts(", ");
    hyp_puthex(he);
    hyp_puts(") from guest stage-2\n");

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

        // --- GICv3 virtualization bring-up ---
        // Own the physical CPU interface at EL2 so physical IRQs (routed here by
        // HCR_EL2.IMO below) can be acked, and enable the virtual CPU interface
        // so we can inject virtual interrupts the guest consumes on ICV_*.
        //
        // EOImode=1 makes our physical EOIR1 a priority-drop only; the actual
        // deactivation is deferred to the guest via hardware-linked list regs.
        msr!("icc_sre_el2", ICC_SRE_SRE | ICC_SRE_ENABLE);
        core::arch::asm!("isb");
        msr!("icc_pmr_el1", 0xFFu64); // accept all priorities (phys)
        let mut ctlr = mrs!("icc_ctlr_el1");
        ctlr |= ICC_CTLR_EOIMODE;
        msr!("icc_ctlr_el1", ctlr);
        msr!("icc_igrpen1_el1", 1u64); // enable phys Group 1 at EL2
        msr!("ich_hcr_el2", ICH_HCR_EN); // enable virtual CPU interface
        core::arch::asm!("isb");

        // Enable stage-2, pin EL1 to AArch64, trap guest ID-register reads
        // (TID3), and route physical IRQs to EL2 (IMO) so we can inject vIRQs.
        msr!("hcr_el2", HCR_RW | HCR_VM | HCR_TID3 | HCR_IMO);
        core::arch::asm!("isb");
    }

    // Register the primary guest (Fermi) as vCPU 0 — its full context is
    // captured lazily on its first yield — and create the tiny guest 1.
    // SAFETY (single-core, pre-guest): exclusive access during boot.
    unsafe {
        let v0 = &mut (*VCPUS.get())[0];
        v0.id = 0;
        v0.state = VCPU_RUNNING;
        v0.vttbr = phys_of(S2_L0.get()); // VMID 0
        *CURRENT_VCPU.get() = 0;
    }
    hyp_create_guest1();
    hyp_puts("[HYP] created guest1 (vCPU 1) with its own stage-2\n");

    hyp_puts("[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

// --- world switch / vCPUs ----------------------------------------------------

/// Save the live EL1 system-register context into `v`.
/// # Safety
/// Reads EL2-accessible EL1 sysregs; `v` must be a valid `Vcpu`.
unsafe fn hyp_save_el1(v: *mut Vcpu) {
    unsafe {
        (*v).sp_el1 = mrs!("sp_el1");
        (*v).elr_el1 = mrs!("elr_el1");
        (*v).spsr_el1 = mrs!("spsr_el1");
        (*v).sctlr_el1 = mrs!("sctlr_el1");
        (*v).cpacr_el1 = mrs!("cpacr_el1");
        (*v).ttbr0_el1 = mrs!("ttbr0_el1");
        (*v).ttbr1_el1 = mrs!("ttbr1_el1");
        (*v).tcr_el1 = mrs!("tcr_el1");
        (*v).mair_el1 = mrs!("mair_el1");
        (*v).amair_el1 = mrs!("amair_el1");
        (*v).vbar_el1 = mrs!("vbar_el1");
        (*v).contextidr_el1 = mrs!("contextidr_el1");
        (*v).tpidr_el1 = mrs!("tpidr_el1");
        (*v).tpidrro_el0 = mrs!("tpidrro_el0");
        (*v).tpidr_el0 = mrs!("tpidr_el0");
        (*v).esr_el1 = mrs!("esr_el1");
        (*v).far_el1 = mrs!("far_el1");
        (*v).par_el1 = mrs!("par_el1");
    }
}

/// Restore `v`'s EL1 system-register context onto the live CPU.
/// # Safety
/// Writes EL1 sysregs from EL2; `v` must be a valid `Vcpu`.
unsafe fn hyp_restore_el1(v: *mut Vcpu) {
    unsafe {
        msr!("sp_el1", (*v).sp_el1);
        msr!("elr_el1", (*v).elr_el1);
        msr!("spsr_el1", (*v).spsr_el1);
        msr!("sctlr_el1", (*v).sctlr_el1);
        msr!("cpacr_el1", (*v).cpacr_el1);
        msr!("ttbr0_el1", (*v).ttbr0_el1);
        msr!("ttbr1_el1", (*v).ttbr1_el1);
        msr!("tcr_el1", (*v).tcr_el1);
        msr!("mair_el1", (*v).mair_el1);
        msr!("amair_el1", (*v).amair_el1);
        msr!("vbar_el1", (*v).vbar_el1);
        msr!("contextidr_el1", (*v).contextidr_el1);
        msr!("tpidr_el1", (*v).tpidr_el1);
        msr!("tpidrro_el0", (*v).tpidrro_el0);
        msr!("tpidr_el0", (*v).tpidr_el0);
        msr!("esr_el1", (*v).esr_el1);
        msr!("far_el1", (*v).far_el1);
        msr!("par_el1", (*v).par_el1);
        core::arch::asm!("isb");
    }
}

/// Round-robin to the next non-unused vCPU after `from`.
fn hyp_pick_next(from: usize) -> usize {
    // SAFETY (single-core): EL2 trap context.
    unsafe {
        for i in 1..=NUM_VCPUS {
            let idx = (from + i) % NUM_VCPUS;
            if (*VCPUS.get())[idx].state != VCPU_UNUSED {
                return idx;
            }
        }
    }
    from
}

/// Cooperative world switch: suspend the running vCPU and resume the next. The
/// outgoing GP regs come from / incoming GP regs go to the EL2 trap frame (which
/// el2_common restores on eret). Each vCPU has a distinct VMID, so no stage-2
/// TLB flush is needed when swapping VTTBR_EL2.
fn hyp_world_switch(frame: *mut El2Frame) {
    // SAFETY (single-core): EL2 trap context has exclusive access to the vCPU
    // table and the trap frame.
    unsafe {
        let from = *CURRENT_VCPU.get();
        let cur = &mut (*VCPUS.get())[from] as *mut Vcpu;

        for i in 0..31 {
            (*cur).regs[i] = (*frame).x[i];
        }
        (*cur).pc = mrs!("elr_el2");
        (*cur).pstate = mrs!("spsr_el2");
        (*cur).vttbr = mrs!("vttbr_el2");
        hyp_save_el1(cur);
        if (*cur).state == VCPU_RUNNING {
            (*cur).state = VCPU_READY;
        }

        let next = hyp_pick_next(from);
        if next == from {
            (*cur).state = VCPU_RUNNING; // nobody else runnable: keep going
            return;
        }

        let nv = &mut (*VCPUS.get())[next] as *mut Vcpu;
        *CURRENT_VCPU.get() = next;
        (*nv).state = VCPU_RUNNING;

        for i in 0..31 {
            (*frame).x[i] = (*nv).regs[i];
        }
        msr!("elr_el2", (*nv).pc);
        msr!("spsr_el2", (*nv).pstate);
        msr!("vttbr_el2", (*nv).vttbr);
        core::arch::asm!("isb");
        hyp_restore_el1(nv);
    }
}

/// Build guest 1's stage-2: identity-map ONLY its RAM slice (IPA == PA over the
/// slice), leaving every other IPA unmapped so the guest is sandboxed.
fn hyp_build_guest1_stage2() {
    let base = phys_of(GUEST1_RAM.get()); // physical == guest IPA
    let pages = GUEST1_RAM_SIZE as u64 / PAGE_SIZE;
    let gb = base / SIZE_1G;
    let mb = (base % SIZE_1G) / SIZE_2M;
    let first_page = (base % SIZE_2M) / PAGE_SIZE;

    let l0 = G1_L0.get();
    let l1 = G1_L1.get();
    let l2 = G1_L2.get();
    let l3 = G1_L3.get();

    // SAFETY (single-core, pre-guest): hypervisor-private tables.
    unsafe {
        for i in 0..512 {
            (*l0).0[i] = 0;
            (*l1).0[i] = 0;
            (*l2).0[i] = 0;
            (*l3).0[i] = 0;
        }
        (*l0).0[0] = phys_of(l1) | S2_TABLE | S2_VALID;
        (*l1).0[gb as usize] = phys_of(l2) | S2_TABLE | S2_VALID;
        (*l2).0[mb as usize] = phys_of(l3) | S2_TABLE | S2_VALID;
        for p in 0..pages {
            let pa = base + p * PAGE_SIZE;
            (*l3).0[(first_page + p) as usize] =
                pa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
        }
        core::arch::asm!("dsb ish");
    }
}

fn hyp_create_guest1() {
    hyp_build_guest1_stage2();

    let payload = core::ptr::addr_of!(guest1_payload) as *const u8;
    let payload_end = core::ptr::addr_of!(guest1_payload_end) as *const u8;
    let base = phys_of(GUEST1_RAM.get());
    // SAFETY (single-core, pre-guest): copy the PIC payload into guest 1's RAM
    // slice; len is the linker-bounded payload size and fits in GUEST1_RAM_SIZE.
    unsafe {
        let len = payload_end as usize - payload as usize;
        core::ptr::copy_nonoverlapping(payload, GUEST1_RAM.get() as *mut u8, len);

        let v = &mut (*VCPUS.get())[1];
        *v = Vcpu::zeroed();
        v.id = 1;
        v.state = VCPU_READY;
        v.pc = base; // entry (IPA == PA)
        v.pstate = 0x3c5; // EL1h, DAIF masked
        v.sp_el1 = base + GUEST1_RAM_SIZE as u64;
        v.vttbr = phys_of(G1_L0.get()) | (1 << 48); // VMID 1
                                                    // EL1 sysregs left 0 => guest 1 runs with its stage-1 MMU off.
    }
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
        EC_SYSREG => "trapped MSR/MRS",
        EC_DABT_LOWER => "data abort (stage-2)",
        EC_IABT_LOWER => "instruction abort (stage-2)",
        _ => "other",
    }
}

/// Read the GP-register slot `i` (x0..x30) of the EL2 trap frame.
#[inline]
fn frame_x(frame: *mut El2Frame, i: usize) -> u64 {
    // SAFETY: `frame` points at the 256-byte stub frame; i is in 0..31.
    unsafe { (*frame).x[i] }
}

/// Write the GP-register slot `i` (x0..x30) of the EL2 trap frame.
#[inline]
fn set_frame_x(frame: *mut El2Frame, i: usize, v: u64) {
    // SAFETY: as `frame_x`.
    unsafe { (*frame).x[i] = v };
}

/// HVC hypercall: function ID in x0, args in x1..x3, result back in x0. ELR_EL2
/// already points past the HVC, so no PC adjustment is needed.
fn hyp_handle_hvc(frame: *mut El2Frame) {
    use hypercall::*;
    let fn_id = frame_x(frame, 0);
    let a1 = frame_x(frame, 1);

    // SAFETY (single-core, pre-guest-concurrency): the vCPU block is touched
    // only from EL2 trap context, which cannot reenter on a single core.
    let vcpu = cur_vcpu();
    unsafe { (*vcpu).hvc_count += 1 };

    let ret = match fn_id {
        HVC_VERSION => HYP_ABI_VERSION,
        HVC_PUTC => {
            hyp_putc(a1 as u8);
            0
        }
        HVC_PING => a1 + 1,
        HVC_VM_INFO => unsafe { (*vcpu).hvc_count },
        HVC_YIELD => {
            // Cooperative world switch: save this vCPU, resume the next ready
            // one. The frame's GP regs are rewritten in place, so we must NOT
            // touch frame x0 afterwards — return immediately.
            hyp_world_switch(frame);
            return;
        }
        // Introspection probe (test build): expose the hyp region base so the
        // guest can attempt — and be denied — an access to hypervisor memory.
        HVC_HYP_BASE => hyp_region().0,
        _ => {
            hyp_puts("[HYP] unknown hypercall fn=");
            hyp_puthex(fn_id);
            hyp_puts("\n");
            HVC_ERR_BADCALL
        }
    };

    set_frame_x(frame, 0, ret);
}

/// Trapped system-register access (EC=0x18), produced here by HCR_EL2.TID3 for
/// guest reads of the ID_AA64* feature registers. We emulate by returning the
/// real value, then step ELR past the trapped instruction (unlike HVC, ELR
/// points *at* it).
///
/// ISS layout for MSR/MRS: Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10]
/// Rt[9:5] CRm[4:1] Direction[0] (1 = read/MRS).
fn hyp_handle_sysreg(frame: *mut El2Frame) {
    let esr = mrs!("esr_el2");
    let iss = esr & 0x1FF_FFFF;
    let op0 = (iss >> 20) & 0x3;
    let op2 = (iss >> 17) & 0x7;
    let op1 = (iss >> 14) & 0x7;
    let crn = (iss >> 10) & 0xF;
    let rt = ((iss >> 5) & 0x1F) as usize;
    let crm = (iss >> 1) & 0xF;
    let is_read = iss & 0x1;

    // SAFETY (single-core): see hyp_handle_hvc.
    unsafe { (*cur_vcpu()).sysreg_traps += 1 };

    // Decode by (op0,op1,crn,crm,op2). Pass real values through for the ID
    // registers Fermi actually consumes; any other ID register under TID3 is
    // architecturally RES0, so returning 0 is safe.
    let val = if op0 == 3 && op1 == 0 && crn == 0 && crm == 4 && op2 == 0 {
        let v = mrs!("id_aa64pfr0_el1");
        hyp_puts("[HYP] emulated guest MRS ID_AA64PFR0_EL1 -> ");
        hyp_puthex(v);
        hyp_puts("\n");
        v
    } else if op0 == 3 && op1 == 0 && crn == 0 && crm == 6 && op2 == 0 {
        mrs!("id_aa64isar0_el1")
    } else if op0 == 3 && op1 == 0 && crn == 0 && crm == 7 && op2 == 0 {
        mrs!("id_aa64mmfr0_el1")
    } else {
        0 // unhandled ID register: RES0
    };

    if is_read != 0 && rt != 31 {
        set_frame_x(frame, rt, val);
    }

    // Skip the trapped instruction.
    // SAFETY: advancing ELR_EL2 past the emulated MRS; EL2 register, no memory.
    unsafe {
        msr!("elr_el2", mrs!("elr_el2") + 4);
    }
}

/// Lower-EL abort. If the guest faulted trying to reach hypervisor-private
/// memory, that's our isolation boundary doing its job: report it, poison the
/// destination register on a read, and step over the access so the guest keeps
/// running. Any other abort is an unexpected (real) fault — dump and park.
fn hyp_handle_abort(index: u64, frame: *mut El2Frame) {
    // SAFETY (single-core): see hyp_handle_hvc.
    unsafe { (*cur_vcpu()).abort_count += 1 };

    let esr = mrs!("esr_el2");
    let far = mrs!("far_el2");
    let hpfar = mrs!("hpfar_el2");
    let ipa_page = (hpfar >> 4) << 12; // HPFAR[43:4] = IPA[51:12]
    let ipa = ipa_page | (far & 0xFFF);

    let (hs, he) = hyp_region();

    if ipa_page >= (hs & !0xFFF) && ipa_page < he {
        let isv = (esr >> 24) & 1; // instruction syndrome valid
        let srt = ((esr >> 16) & 0x1F) as usize; // destination register
        let wnr = (esr >> 6) & 1; // write (1) vs read (0)

        hyp_puts("\n[HYP] ISOLATION: blocked guest ");
        hyp_puts(if wnr != 0 { "write to" } else { "read from" });
        hyp_puts(" hyp memory IPA=");
        hyp_puthex(ipa);
        hyp_puts("\n");

        if wnr == 0 && isv != 0 && srt != 31 {
            set_frame_x(frame, srt, 0); // poison value for the blocked read
        }

        // Step past the faulting instruction so the guest keeps running.
        // SAFETY: advancing ELR_EL2; EL2 register, no memory effect.
        unsafe {
            msr!("elr_el2", mrs!("elr_el2") + 4);
        }
        return;
    }

    // Not the isolation boundary — a real, unexpected fault. Dump once and park
    // (the DUMPED guard keeps a re-fault from spamming the log).
    if !DUMPED.swap(true, Ordering::Relaxed) {
        hyp_puts("\n[HYP] *** unexpected lower-EL abort *** vector=");
        hyp_puthex(index);
        hyp_puts(" EC=");
        hyp_puthex((esr >> ESR_EC_SHIFT) & ESR_EC_MASK);
        hyp_puts("\n      ESR_EL2=");
        hyp_puthex(esr);
        hyp_puts(" ELR_EL2=");
        hyp_puthex(mrs!("elr_el2"));
        hyp_puts("\n      FAR_EL2=");
        hyp_puthex(far);
        hyp_puts(" faulting IPA=");
        hyp_puthex(ipa);
        hyp_puts("\n[HYP] parking CPU for inspection.\n");
        loop {
            unsafe { core::arch::asm!("wfi") };
        }
    }
    hyp_puts("[HYP] unhandled EL2 exception (continuing)\n\n");
}

// --- vGIC --------------------------------------------------------------------

/// Inject a hardware-linked virtual interrupt into the guest via a free list
/// register. HW=1 ties the virtual INTID to the physical one so the guest's own
/// EOI/deactivation on the virtual interface deactivates the physical interrupt
/// — no maintenance interrupt needed. Only the timer PPI is in play for this
/// guest, so a single in-flight LR is the normal case; we scan LR0/LR1 for
/// robustness.
fn hyp_vgic_inject_hw(intid: u32) {
    let lr = (intid as u64)                              // vINTID [31:0]
        | ((intid as u64) << ICH_LR_PINTID_SHIFT)        // pINTID
        | (0u64 << ICH_LR_PRIO_SHIFT)                    // priority 0
        | ICH_LR_GROUP1
        | ICH_LR_HW
        | ICH_LR_STATE_PENDING;

    // SAFETY: ICH_LR<n>_EL2 are EL2 vGIC list registers; reads/writes here have
    // no memory side effects. State[63:62]==0 (Invalid) means the LR is free.
    unsafe {
        let lr0 = mrs!("ich_lr0_el2");
        if (lr0 >> 62) == 0 {
            msr!("ich_lr0_el2", lr);
            return;
        }
        let lr1 = mrs!("ich_lr1_el2");
        if (lr1 >> 62) == 0 {
            msr!("ich_lr1_el2", lr);
            return;
        }
        // No free LR (shouldn't happen with a single timer source): overwrite
        // LR0 as a best-effort fallback.
        msr!("ich_lr0_el2", lr);
    }
}

/// Physical IRQ taken at EL2 (HCR_EL2.IMO). Ack on the physical CPU interface,
/// inject a hardware-linked virtual copy into the guest, then priority-drop
/// (EOImode=1 means this does not deactivate — the guest will, via the HW link).
/// The guest's existing IRQ handler runs unmodified on ICV_*.
fn hyp_handle_irq() {
    let iar = mrs!("icc_iar1_el1");
    let intid = (iar & 0xFF_FFFF) as u32;

    if intid >= 1020 {
        // 1020-1023 are special / spurious: no EOI needed.
        return;
    }

    // SAFETY (single-core): see hyp_handle_hvc.
    let n = unsafe {
        let v = cur_vcpu();
        (*v).virq_injected += 1;
        (*v).virq_injected
    };
    hyp_vgic_inject_hw(intid);
    // Priority drop on the physical interface (does not deactivate).
    // SAFETY: EOIR1 write on the physical CPU interface.
    unsafe { msr!("icc_eoir1_el1", iar) };

    if n <= 3 {
        hyp_puts("[HYP] injected hw vIRQ intid=");
        hyp_puthex(intid as u64);
        hyp_puts(" (count=");
        hyp_puthex(n);
        hyp_puts(")\n");
    }
}

/// Dispatcher for EL2 exceptions. `index` is the vector slot (0..15); 8 = sync
/// from a lower EL (AArch64), which is where guest HVC/aborts land; slot kind 1
/// = IRQ.
#[no_mangle]
pub extern "C" fn el2_dispatch(index: u64, frame: *mut El2Frame) {
    // Vector slot kind: 0=sync, 1=IRQ, 2=FIQ, 3=SError (within each group).
    let kind = index & 3;
    if kind == 1 {
        hyp_handle_irq();
        return;
    }
    if kind != 0 {
        hyp_puts("\n[HYP] unexpected EL2 exception kind=");
        hyp_puthex(kind);
        hyp_puts(" vector=");
        hyp_puthex(index);
        hyp_puts("\n");
        return;
    }

    let ec = (mrs!("esr_el2") >> ESR_EC_SHIFT) & ESR_EC_MASK;

    match ec {
        EC_HVC64 => hyp_handle_hvc(frame),
        EC_SYSREG => hyp_handle_sysreg(frame),
        EC_DABT_LOWER | EC_IABT_LOWER => hyp_handle_abort(index, frame),
        _ => {
            hyp_puts("\n[HYP] unhandled EL2 exception: vector=");
            hyp_puthex(index);
            hyp_puts(" EC=");
            hyp_puthex(ec);
            hyp_puts(" (");
            hyp_puts(ec_name(ec));
            hyp_puts(") ELR_EL2=");
            hyp_puthex(mrs!("elr_el2"));
            hyp_puts("\n");
        }
    }
}

/// Guest-side hypercall probe (runs at EL1). Exercises the ABI so M2 is
/// observable from a boot log. Gated on [`booted_via_el2`]: on a bare EL1 boot
/// an `HVC` would trap as UNDEFINED.
pub fn guest_probe() {
    use crate::{kprint, kprintln};
    use hypercall::*;

    if !booted_via_el2() {
        return;
    }

    kprintln!("[BOOT] testing hypervisor calls...");
    // SAFETY: gated on booted_via_el2(), so a hypervisor is present at EL2.
    unsafe {
        kprintln!(
            "[BOOT]   HVC_VERSION  -> {:#x}",
            hvc_call(HVC_VERSION, 0, 0, 0)
        );
        kprintln!("[BOOT]   HVC_PING(41) -> {}", hvc_call(HVC_PING, 41, 0, 0));
        kprint!("[BOOT]   HVC_PUTC ->");
        hvc_call(HVC_PUTC, b' ' as u64, 0, 0);
        hvc_call(HVC_PUTC, b'h' as u64, 0, 0);
        hvc_call(HVC_PUTC, b'i' as u64, 0, 0);
        hvc_call(HVC_PUTC, b'\n' as u64, 0, 0);
        kprintln!(
            "[BOOT]   HVC_VM_INFO  -> {} hypercalls served",
            hvc_call(HVC_VM_INFO, 0, 0, 0)
        );

        // Isolation probe: ask the hypervisor where its private memory lives,
        // then deliberately try to read it. Stage-2 must block this — the read
        // should trap to EL2 and come back poisoned (0) rather than leaking hyp
        // state. Runs while TTBR0 still identity-maps the low half, so the
        // guest VA == IPA and the stage-2 hole takes the fault.
        let hyp_base = hvc_call(HVC_HYP_BASE, 0, 0, 0);
        kprintln!(
            "[BOOT] poking hypervisor memory at {:#x} (must be blocked)...",
            hyp_base
        );
        let leaked = read_volatile(hyp_base as *const u64);
        kprintln!(
            "[BOOT]   read back {:#x} ({})",
            leaked,
            if leaked == 0 {
                "0 => stage-2 isolation held"
            } else {
                "LEAK => isolation FAILED"
            }
        );

        // Cooperative multi-VM demo: yield to guest 1 a few times. Each yield
        // world-switches to the second guest, which prints "[g1]" and yields
        // back. After the loop guest 0 keeps the CPU (no preemption yet — M5b).
        kprintln!("[BOOT] cooperative multi-VM demo (guest0 <-> guest1):");
        for i in 0..4 {
            kprintln!("[BOOT][guest0] turn {}, yielding to guest1...", i);
            hvc_call(HVC_YIELD, 0, 0, 0);
        }
        kprintln!("[BOOT][guest0] done yielding; resuming normal boot");
    }
}
