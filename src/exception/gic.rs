//! GICv3 interrupt controller — minimal bringup.
//!
//! Port of `src/exception/gic/gic.c`: distributor + redistributor init,
//! affinity routing, system-register CPU interface, IRQ ack/EOI, and
//! per-INTID counters for /proc/interrupts.

use crate::kprintln;
use crate::mmio;
use crate::sync::Racy;
use crate::uart;

const GICD_BASE: usize = 0x0800_0000;
const GICR_BASE: usize = 0x080A_0000;

const GICD_CTLR: usize = GICD_BASE + 0x0000;
const GICD_ISENABLER: usize = GICD_BASE + 0x0100;
const GICD_IGROUPR: usize = GICD_BASE + 0x0080;
const GICD_IPRIORITYR: usize = GICD_BASE + 0x0400;
const GICD_IROUTER: usize = GICD_BASE + 0x6000;

const GICD_CTLR_ENABLE_G1NS: u32 = 1 << 1;
const GICD_CTLR_ARE_NS: u32 = 1 << 4;

const GICR_WAKER: usize = GICR_BASE + 0x0014;
const GICR_SGI_BASE: usize = GICR_BASE + 0x10000;
const GICR_IGROUPR0: usize = GICR_SGI_BASE + 0x0080;
const GICR_IGRPMODR0: usize = GICR_SGI_BASE + 0x0D00;
const GICR_ISENABLER0: usize = GICR_SGI_BASE + 0x0100;
const GICR_WAKER_PROCESSOR_SLEEP: u32 = 1 << 1;
const GICR_WAKER_CHILDREN_ASLEEP: u32 = 1 << 2;

pub const GIC_INTID_NO_PENDING: u64 = 1023;

const GIC_COUNTERS_MAX: usize = 256;
static IRQ_COUNTS: Racy<[u64; GIC_COUNTERS_MAX]> = Racy::new([0; GIC_COUNTERS_MAX]);

fn enable_system_register_interface() {
    uart::println("[GIC] Enabling System Register Interface");
    let mut sre: u64 = crate::mrs!(icc_sre_el1);
    sre |= 1;
    crate::msr!(icc_sre_el1, sre);
    unsafe { core::arch::asm!("isb") };
}

fn enable_distributor_affinity_routing() {
    uart::println("[GIC] Enabling Distributor affinity routing");
    mmio::write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
    kprintln!("[GIC] GICD_CTLR = {:#x}", mmio::read32(GICD_CTLR));
}

fn redistributor_wakeup() {
    let mut waker = mmio::read32(GICR_WAKER);
    waker &= !GICR_WAKER_PROCESSOR_SLEEP;
    mmio::write32(GICR_WAKER, waker);
    while mmio::read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP != 0 {}
    uart::println("[GIC] Redistributor awake");
}

pub fn init() {
    uart::println("[GIC] Initializing GICv3");
    enable_system_register_interface();
    enable_distributor_affinity_routing();
    redistributor_wakeup();

    // All SGIs/PPIs (0-31) as Group 1 Non-secure.
    mmio::write32(GICR_IGROUPR0, 0xFFFF_FFFF);
    mmio::write32(GICR_IGRPMODR0, 0x0000_0000);

    // Priority mask: accept all priorities.
    crate::msr!(icc_pmr_el1, 0xFF);
    // Enable Group 1 interrupts at the CPU interface.
    crate::msr!(icc_igrpen1_el1, 0x01);
    unsafe { core::arch::asm!("isb") };

    // Unmask IRQs (DAIF.I).
    unsafe { core::arch::asm!("msr daifclr, #2") };

    uart::println("[GIC] Initialized! IRQs enabled");
}

pub fn enable_irq(intid: u32) {
    if intid < 32 {
        let mut val = mmio::read32(GICR_ISENABLER0);
        val |= 1 << intid;
        mmio::write32(GICR_ISENABLER0, val);
    } else {
        // SPI: on GICv3 with affinity routing, configure Group1NS, priority,
        // and route to PE affinity 0 before enabling — otherwise the
        // interrupt is never delivered to a CPU.
        let n = intid as usize / 32;
        let bit = intid % 32;
        // Group 1 (non-secure).
        let gr = GICD_IGROUPR + n * 4;
        mmio::write32(gr, mmio::read32(gr) | (1 << bit));
        // Priority (byte per INTID); 0xA0 = mid priority, below PMR 0xFF.
        mmio::write8(GICD_IPRIORITYR + intid as usize, 0xA0);
        // Route to PE with affinity 0.0.0.0 (IRM=0).
        mmio::write32(GICD_IROUTER + intid as usize * 8, 0);
        mmio::write32(GICD_IROUTER + intid as usize * 8 + 4, 0);
        // Enable.
        let reg = GICD_ISENABLER + n * 4;
        mmio::write32(reg, mmio::read32(reg) | (1 << bit));
    }
    kprintln!("[GIC] Enabled IRQ {}", intid);
}

pub fn ack_irq() -> u64 {
    crate::mrs!(icc_iar1_el1)
}

pub fn end_irq(intid: u64) {
    crate::msr!(icc_eoir1_el1, intid);
}

pub fn count_irq(intid: u32) {
    if (intid as usize) < GIC_COUNTERS_MAX {
        unsafe { IRQ_COUNTS.get()[intid as usize] += 1 };
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

/// Render the /proc/interrupts table into a String.
pub fn render_interrupts() -> alloc::string::String {
    use core::fmt::Write;
    let mut s = alloc::string::String::new();
    let _ = writeln!(s, "INTID  COUNT     SOURCE");
    let counts = unsafe { IRQ_COUNTS.get() };
    for i in 0..GIC_COUNTERS_MAX {
        if counts[i] == 0 {
            continue;
        }
        let _ = writeln!(s, "{}    {}  {}", i, counts[i], intid_source(i as u32));
    }
    s
}
