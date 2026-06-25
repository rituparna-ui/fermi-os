//! VirtIO PCI transport: device status bits, common-config register offsets,
//! and capability-list parsing (vendor-specific cap 0x09) to resolve the
//! common/notify/ISR/device config windows. Shared by all VirtIO drivers.

#![allow(dead_code)]

pub mod balloon;
pub mod blk;
pub mod console;
pub mod net;
pub mod rng;
pub mod virtqueue;

use crate::drivers::pci::{self, PciDevice, PCI_CAP_PTR};
use crate::klib::uart::Uart;
use crate::kprintln;

// Device status bits.
pub const VIRTIO_STATUS_RESET: u8 = 0;
pub const VIRTIO_STATUS_ACKNOWLEDGE: u8 = 1;
pub const VIRTIO_STATUS_DRIVER: u8 = 2;
pub const VIRTIO_STATUS_DRIVER_OK: u8 = 4;
pub const VIRTIO_STATUS_FEATURES_OK: u8 = 8;
pub const VIRTIO_STATUS_FAILED: u8 = 128;

// VirtIO PCI common-config register offsets (spec 4.1.4.3).
pub const VIRTIO_COMMON_DFSELECT: usize = 0x00;
pub const VIRTIO_COMMON_DF: usize = 0x04;
pub const VIRTIO_COMMON_GFSELECT: usize = 0x08;
pub const VIRTIO_COMMON_GF: usize = 0x0C;
pub const VIRTIO_COMMON_MSIX: usize = 0x10;
pub const VIRTIO_COMMON_NUMQ: usize = 0x12;
pub const VIRTIO_COMMON_STATUS: usize = 0x14;
pub const VIRTIO_COMMON_CFGGEN: usize = 0x15;
pub const VIRTIO_COMMON_Q_SELECT: usize = 0x16;
pub const VIRTIO_COMMON_Q_SIZE: usize = 0x18;
pub const VIRTIO_COMMON_Q_MSIX: usize = 0x1A;
pub const VIRTIO_COMMON_Q_ENABLE: usize = 0x1C;
pub const VIRTIO_COMMON_Q_NOFF: usize = 0x1E;
pub const VIRTIO_COMMON_Q_DESCLO: usize = 0x20;
pub const VIRTIO_COMMON_Q_DESCHI: usize = 0x24;
pub const VIRTIO_COMMON_Q_DRIVERLO: usize = 0x28;
pub const VIRTIO_COMMON_Q_DRIVERHI: usize = 0x2C;
pub const VIRTIO_COMMON_Q_DEVICELO: usize = 0x30;
pub const VIRTIO_COMMON_Q_DEVICEHI: usize = 0x34;

const PCI_STATUS: u16 = 0x06;

/// Resolved VirtIO PCI capability windows (physical addresses).
#[derive(Clone, Copy, Default)]
pub struct VirtioPciCaps {
    pub common_cfg: usize,
    pub notify_base: usize,
    pub isr_cfg: usize,
    pub device_cfg: usize,
    /// notify addr for queue i = notify_base + queue_notify_off[i] * mult.
    pub notify_off_multiplier: u32,
}

fn cfg_type_name(t: u8) -> &'static str {
    match t {
        1 => "COMMON_CFG",
        2 => "NOTIFY_CFG",
        3 => "ISR_CFG",
        4 => "DEVICE_CFG",
        5 => "PCI_CFG",
        _ => "UNKNOWN",
    }
}

fn populate_capability(dev: &PciDevice, caps: &mut VirtioPciCaps, cap_ptr: u8) {
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let cp = cap_ptr as u16;

    // virtio_pci_cap layout (spec 4.1.4): +3 cfg_type, +4 bar, +8 offset,
    // +0x0C length.
    let cfg_type = pci::config_read8(b, d, f, cp + 3);
    let bar = pci::config_read8(b, d, f, cp + 4);
    let offset = pci::config_read32(b, d, f, cp + 8);
    let length = pci::config_read32(b, d, f, cp + 0x0C);

    if bar >= 6 {
        Uart.errorln("[VIRTIO] populate_capabilities: invalid BAR index");
        return;
    }

    let cap_addr = (dev.bar_addr[bar as usize] + offset as u64) as usize;
    kprintln!(
        "  type={} ({}) bar={} offset={:#x} length={:#x} -> addr={:#x}",
        cfg_type,
        cfg_type_name(cfg_type),
        bar,
        offset,
        length,
        cap_addr
    );

    match cfg_type {
        1 => caps.common_cfg = cap_addr,
        2 => {
            caps.notify_base = cap_addr;
            caps.notify_off_multiplier = pci::config_read32(b, d, f, cp + 0x10);
            kprintln!("  notify_off_multiplier={:#x}", caps.notify_off_multiplier);
        }
        3 => caps.isr_cfg = cap_addr,
        4 => caps.device_cfg = cap_addr,
        _ => {}
    }
}

/// Run the common VirtIO device init handshake on the common-cfg at `base`:
/// reset → ack → driver → feature negotiation → FEATURES_OK. `extra_features_hi`
/// lets a driver accept device-specific feature bits in the high word
/// (VIRTIO_F_VERSION_1 = bit 0 of the high word is always added). `tag` is a
/// short driver name used for logging. Returns the negotiated high features on
/// success, or None on failure.
pub fn device_init_handshake(base: usize, tag: &str, extra_features_hi: u32) -> Option<u32> {
    use crate::arch::cpu::dsb_sy;
    use crate::klib::mmio;

    // Reset and wait for it to take.
    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}

    // ACK + DRIVER.
    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(
        base + VIRTIO_COMMON_STATUS,
        status | VIRTIO_STATUS_ACKNOWLEDGE,
    );
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    // Read device features.
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 0);
    dsb_sy();
    let feat_lo = mmio::read32(base + VIRTIO_COMMON_DF);
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 1);
    dsb_sy();
    let feat_hi = mmio::read32(base + VIRTIO_COMMON_DF);
    kprintln!(
        "[{}] device features: lo={:#x} hi={:#x}",
        tag,
        feat_lo,
        feat_hi
    );

    // Accept VIRTIO_F_VERSION_1 (high bit 0) plus any requested extra high bits
    // the device actually offers.
    let guest_lo = 0u32;
    let guest_hi = (feat_hi & 0x01) | (feat_hi & extra_features_hi);
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_lo);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 1);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_hi);
    dsb_sy();
    kprintln!(
        "[{}] accepted features: lo={:#x} hi={:#x}",
        tag,
        guest_lo,
        guest_hi
    );

    // FEATURES_OK + verify it stuck.
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(
        base + VIRTIO_COMMON_STATUS,
        status | VIRTIO_STATUS_FEATURES_OK,
    );
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    if status & VIRTIO_STATUS_FEATURES_OK == 0 {
        Uart.errorln("[VIRTIO] FEATURES_OK failed");
        return None;
    }
    kprintln!("[{}] FEATURES_OK (status={:#x})", tag, status);
    Some(guest_hi)
}

/// Set DRIVER_OK on the device at common-cfg `base`.
pub fn set_driver_ok(base: usize) {
    use crate::arch::cpu::dsb_sy;
    use crate::klib::mmio;
    let status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(
        base + VIRTIO_COMMON_STATUS,
        status | VIRTIO_STATUS_DRIVER_OK,
    );
    dsb_sy();
}

/// Walk the device's capability list, resolving VirtIO config windows.
pub fn parse_capabilities(dev: &PciDevice, caps: &mut VirtioPciCaps) {
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let status = pci::config_read16(b, d, f, PCI_STATUS);
    if status & (1 << 4) == 0 {
        Uart.errorln("[PCI] Capabilities not present");
        return;
    }

    let mut cap_ptr = pci::config_read8(b, d, f, PCI_CAP_PTR);
    while cap_ptr != 0 {
        let cap_id = pci::config_read8(b, d, f, cap_ptr as u16);
        let next = pci::config_read8(b, d, f, cap_ptr as u16 + 1);

        Uart.puts("[PCI] CapId: ");
        Uart.puthex(cap_id as u64);
        match cap_id {
            0x11 => Uart.println(" MSI-X 0x11. Ignoring for now"),
            0x09 => {
                Uart.println(" Vendor specific 0x09");
                populate_capability(dev, caps, cap_ptr);
            }
            _ => Uart.println(" Other Cap Type"),
        }
        cap_ptr = next;
    }
}
