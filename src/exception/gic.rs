//! GICv3 interrupt controller — minimal bring-up for the QEMU `virt` machine.
//!
//! Initializes the distributor (affinity routing + Group-1-NS), wakes the
//! redistributor, configures SGIs/PPIs as Group-1-NS, and enables the system
//! register CPU interface (ICC_*). Provides IRQ enable, ack (IAR1), EOI (EOIR1),
//! and per-INTID counters for `/proc/interrupts`.

use crate::klib::mmio;
use crate::klib::sync::SpinLock;
use crate::kprintln;
use crate::msr;

const GICD_BASE: usize = 0x0800_0000;
const GICR_BASE: usize = 0x080A_0000;

const GICD_CTLR: usize = GICD_BASE + 0x0000;
const GICD_ISENABLER: usize = GICD_BASE + 0x0100;

const GICD_CTLR_ENABLE_G1NS: u32 = 1 << 1;
const GICD_CTLR_ARE_NS: u32 = 1 << 4;

const GICR_WAKER: usize = GICR_BASE + 0x0014;
const GICR_SGI_BASE: usize = GICR_BASE + 0x10000;
const GICR_IGROUPR0: usize = GICR_SGI_BASE + 0x0080;
const GICR_IGRPMODR0: usize = GICR_SGI_BASE + 0x0D00;
const GICR_ISENABLER0: usize = GICR_SGI_BASE + 0x0100;
const GICR_WAKER_PROCESSOR_SLEEP: u32 = 1 << 1;
const GICR_WAKER_CHILDREN_ASLEEP: u32 = 1 << 2;

/// Spurious INTID returned by IAR1 when there's no pending interrupt.
pub const GIC_INTID_NO_PENDING: u64 = 1023;

fn enable_system_register_interface() {
    kprintln!("[GIC] Enabling System Register Interface");
    let mut sre = mrs!("icc_sre_el1");
    sre |= 1;
    unsafe {
        msr!("icc_sre_el1", sre);
        core::arch::asm!("isb");
    }
}

fn enable_distributor_affinity_routing() {
    kprintln!("[GIC] Enabling Distributor affinity routing");
    mmio::write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
    kprintln!("[GIC] GICD_CTLR = {:#x}", mmio::read32(GICD_CTLR));
}

fn redistributor_wakeup() {
    let mut waker = mmio::read32(GICR_WAKER);
    waker &= !GICR_WAKER_PROCESSOR_SLEEP;
    mmio::write32(GICR_WAKER, waker);
    // Poll until ChildrenAsleep clears.
    while mmio::read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP != 0 {}
    kprintln!("[GIC] Redistributor awake");
}

/// Bring up the GICv3 and unmask IRQs at the PSTATE level.
pub fn init() {
    kprintln!("[GIC] Initializing GICv3");

    enable_system_register_interface();
    enable_distributor_affinity_routing();
    redistributor_wakeup();

    // Mark all SGIs/PPIs (0-31) as Group-1 Non-secure.
    mmio::write32(GICR_IGROUPR0, 0xFFFF_FFFF);
    mmio::write32(GICR_IGRPMODR0, 0x0000_0000);

    unsafe {
        // Priority mask: accept all priorities.
        msr!("icc_pmr_el1", 0xFF);
        // Enable Group-1 interrupts at the CPU interface.
        msr!("icc_igrpen1_el1", 0x01);
        core::arch::asm!("isb");
        // Unmask IRQs (DAIF.I).
        core::arch::asm!("msr daifclr, #2");
    }

    kprintln!("[GIC] Initialized! IRQs enabled");
}

/// Enable a specific interrupt ID (SGI/PPI via redistributor, SPI via dist).
pub fn enable_irq(intid: u32) {
    if intid < 32 {
        let val = mmio::read32(GICR_ISENABLER0) | (1u32 << intid);
        mmio::write32(GICR_ISENABLER0, val);
    } else {
        let reg = GICD_ISENABLER + (intid as usize / 32) * 4;
        let bit = intid % 32;
        let val = mmio::read32(reg) | (1u32 << bit);
        mmio::write32(reg, val);
    }
    kprintln!("[GIC] Enabled IRQ {}", intid);
}

/// Acknowledge the highest-priority pending Group-1 interrupt (reads IAR1).
pub fn ack_irq() -> u64 {
    mrs!("icc_iar1_el1")
}

/// Signal end-of-interrupt for `intid` (writes EOIR1).
pub fn end_irq(intid: u64) {
    unsafe {
        msr!("icc_eoir1_el1", intid);
    }
}

// Per-INTID counters for /proc/interrupts. 256 entries cover the QEMU virt
// machine's SGI/PPI/SPI range; INTIDs >= 256 are silently not counted.
const GIC_COUNTERS_MAX: usize = 256;
static IRQ_COUNTS: SpinLock<[u64; GIC_COUNTERS_MAX]> = SpinLock::new([0; GIC_COUNTERS_MAX]);

/// Count an acknowledged interrupt (call after ack so spurious 1023 is skipped).
pub fn count_irq(intid: u32) {
    if (intid as usize) < GIC_COUNTERS_MAX {
        IRQ_COUNTS.lock()[intid as usize] += 1;
    }
}

fn intid_source(intid: u32) -> &'static str {
    if intid == 30 {
        "timer (PPI)"
    } else if intid < 16 {
        "SGI"
    } else if intid < 32 {
        "PPI"
    } else {
        "SPI"
    }
}

/// Render a `/proc/interrupts`-style table into `out`, returning bytes written.
pub fn render_interrupts(out: &mut [u8]) -> usize {
    use core::fmt::Write;
    let mut w = crate::klib::fmtbuf::FmtBuf::new(out);
    let _ = w.write_str("INTID  COUNT     SOURCE\n");
    let counts = IRQ_COUNTS.lock();
    for (i, &c) in counts.iter().enumerate() {
        if c == 0 {
            continue;
        }
        let _ = write!(w, "{}    {}  {}\n", i, c, intid_source(i as u32));
    }
    w.len()
}
