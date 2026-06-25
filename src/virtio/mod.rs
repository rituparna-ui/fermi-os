//! VirtIO PCI transport — capability walking + common-config register map.
//!
//! Port of `src/pci/virtio/virtio.c`.

pub mod virtqueue;
pub mod rng;
pub mod blk;
pub mod net;

use crate::kprintln;
use crate::pci::{self, PciDevice};
use crate::uart;

// Device status bits.
pub const VIRTIO_STATUS_RESET: u8 = 0;
pub const VIRTIO_STATUS_ACKNOWLEDGE: u8 = 1;
pub const VIRTIO_STATUS_DRIVER: u8 = 2;
pub const VIRTIO_STATUS_DRIVER_OK: u8 = 4;
pub const VIRTIO_STATUS_FEATURES_OK: u8 = 8;
pub const VIRTIO_STATUS_FAILED: u8 = 128;

// Common-config register offsets (virtio 4.1.4.3).
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

#[derive(Clone, Copy, Default)]
pub struct VirtioPciCaps {
    pub common_cfg: usize,
    pub notify_base: usize,
    pub isr_cfg: usize,
    pub device_cfg: usize,
    pub notify_off_multiplier: u32,
}

#[inline(always)]
pub fn dsb_sy() {
    unsafe { core::arch::asm!("dsb sy") };
}

fn populate(dev: &PciDevice, caps: &mut VirtioPciCaps, cap_ptr: u16) {
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let cfg_type = pci::config_read8(b, d, f, cap_ptr + 3);
    let bar = pci::config_read8(b, d, f, cap_ptr + 4);
    let offset = pci::config_read32(b, d, f, cap_ptr + 8);
    if bar >= 6 {
        uart::errorln("[VIRTIO] invalid BAR index in cap");
        return;
    }
    let cap_addr = dev.bar_addr[bar as usize] as usize + offset as usize;
    match cfg_type {
        1 => caps.common_cfg = cap_addr,
        2 => {
            caps.notify_base = cap_addr;
            caps.notify_off_multiplier = pci::config_read32(b, d, f, cap_ptr + 0x10);
        }
        3 => caps.isr_cfg = cap_addr,
        4 => caps.device_cfg = cap_addr,
        _ => {}
    }
    kprintln!(
        "[VIRTIO]  cap type={} bar={} offset={:#x} -> {:#x}",
        cfg_type,
        bar,
        offset,
        cap_addr
    );
}

/// Walk the PCI capability list and resolve VirtIO config BAR+offsets.
pub fn parse_capabilities(dev: &PciDevice, caps: &mut VirtioPciCaps) {
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let status = pci::config_read16(b, d, f, pci::PCI_STATUS);
    if status & (1 << 4) == 0 {
        uart::errorln("[PCI] Capabilities not present");
        return;
    }
    let mut cap_ptr = pci::config_read8(b, d, f, pci::PCI_CAP_PTR) as u16;
    while cap_ptr != 0 {
        let cap_id = pci::config_read8(b, d, f, cap_ptr);
        let next = pci::config_read8(b, d, f, cap_ptr + 1) as u16;
        if cap_id == 0x09 {
            populate(dev, caps, cap_ptr);
        }
        cap_ptr = next;
    }
}
