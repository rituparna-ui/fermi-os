//! PCI Express (ECAM) — brute-force enumeration, BAR assignment, enable.
//!
//! Port of `src/pci/pci.c` for the QEMU virt machine.

use crate::kprintln;
use crate::mmio;
use crate::sync::Racy;
use crate::uart;

pub const PCI_ECAM_PHYS: usize = 0x40_1000_0000;
pub const PCI_MMIO32_PHYS: u64 = 0x1000_0000;
pub const PCI_MMIO32_LIMIT: u64 = 0x3EFE_FFFF;
pub const PCI_MMIO64_PHYS: u64 = 0x80_0000_0000;
pub const PCI_MMIO64_LIMIT: u64 = 0xFF_FFFF_FFFF;

const MAX_PCI_DEVICES: usize = 16;
const MAX_PCI_BUS: u16 = 256;
const MAX_PCI_SLOT: u8 = 32;
const MAX_PCI_FUNC: u8 = 8;

pub const PCI_VENDOR_ID: u16 = 0x00;
pub const PCI_DEVICE_ID: u16 = 0x02;
pub const PCI_COMMAND: u16 = 0x04;
pub const PCI_STATUS: u16 = 0x06;
pub const PCI_HEADER_TYPE: u16 = 0x0E;
pub const PCI_BAR0: u16 = 0x10;
pub const PCI_CAP_PTR: u16 = 0x34;
pub const PCI_INTERRUPT_PIN: u16 = 0x3D;
pub const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

#[derive(Clone, Copy)]
pub struct PciDevice {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub bar_addr: [u64; 6],
}

impl PciDevice {
    const fn empty() -> Self {
        Self {
            bus: 0,
            slot: 0,
            func: 0,
            vendor_id: 0,
            device_id: 0,
            bar_addr: [0; 6],
        }
    }
}

struct PciState {
    devices: [PciDevice; MAX_PCI_DEVICES],
    count: u16,
    mmio32_next: u64,
    mmio64_next: u64,
}

static PCI: Racy<PciState> = Racy::new(PciState {
    devices: [PciDevice::empty(); MAX_PCI_DEVICES],
    count: 0,
    mmio32_next: PCI_MMIO32_PHYS,
    mmio64_next: PCI_MMIO64_PHYS,
});

fn ecam_addr(bus: u16, slot: u8, func: u8, offset: u16) -> usize {
    PCI_ECAM_PHYS
        | ((bus as usize) << 20)
        | ((slot as usize) << 15)
        | ((func as usize) << 12)
        | offset as usize
}

pub fn config_read32(bus: u16, slot: u8, func: u8, off: u16) -> u32 {
    mmio::read32(ecam_addr(bus, slot, func, off))
}
pub fn config_read16(bus: u16, slot: u8, func: u8, off: u16) -> u16 {
    mmio::read16(ecam_addr(bus, slot, func, off))
}
pub fn config_read8(bus: u16, slot: u8, func: u8, off: u16) -> u8 {
    mmio::read8(ecam_addr(bus, slot, func, off))
}
pub fn config_write32(bus: u16, slot: u8, func: u8, off: u16, v: u32) {
    mmio::write32(ecam_addr(bus, slot, func, off), v)
}
pub fn config_write16(bus: u16, slot: u8, func: u8, off: u16, v: u16) {
    mmio::write16(ecam_addr(bus, slot, func, off), v)
}

pub fn enumerate_bus() {
    uart::println("[PCI] Enumerating PCI Devices");
    let p = unsafe { PCI.get() };
    for bus in 0..MAX_PCI_BUS {
        for slot in 0..MAX_PCI_SLOT {
            for func in 0..MAX_PCI_FUNC {
                let vendor_id = config_read16(bus, slot, func, PCI_VENDOR_ID);
                if vendor_id == 0xFFFF {
                    continue;
                }
                let device_id = config_read16(bus, slot, func, PCI_DEVICE_ID);
                kprintln!(
                    "[PCI] Device at {}:{}.{} | Vendor: {:#x} Device: {:#x}",
                    bus,
                    slot,
                    func,
                    vendor_id,
                    device_id
                );
                if p.count as usize >= MAX_PCI_DEVICES {
                    uart::errorln("[PCI] Max PCI devices reached");
                    return;
                }
                p.devices[p.count as usize] = PciDevice {
                    bus: bus as u8,
                    slot,
                    func,
                    vendor_id,
                    device_id,
                    bar_addr: [0; 6],
                };
                p.count += 1;
            }
        }
    }
}

/// Find a device by vendor/device id (returns a copy).
pub fn find_device(vendor_id: u16, device_id: u16) -> Option<PciDevice> {
    let p = unsafe { PCI.get() };
    for i in 0..p.count as usize {
        if p.devices[i].vendor_id == vendor_id && p.devices[i].device_id == device_id {
            return Some(p.devices[i]);
        }
    }
    None
}

pub fn header_type(dev: &PciDevice) -> u8 {
    config_read8(dev.bus as u16, dev.slot, dev.func, PCI_HEADER_TYPE)
}

fn bar_size32(bus: u16, slot: u8, func: u8, off: u16) -> u32 {
    let original = config_read32(bus, slot, func, off);
    config_write32(bus, slot, func, off, 0xFFFF_FFFF);
    let mut mask = config_read32(bus, slot, func, off);
    config_write32(bus, slot, func, off, original);
    mask &= !0xF;
    if mask == 0 {
        return 0;
    }
    (!mask).wrapping_add(1)
}

fn bar_size64(bus: u16, slot: u8, func: u8, lo: u16, hi: u16) -> u64 {
    let orig_lo = config_read32(bus, slot, func, lo);
    let orig_hi = config_read32(bus, slot, func, hi);
    config_write32(bus, slot, func, lo, 0xFFFF_FFFF);
    config_write32(bus, slot, func, hi, 0xFFFF_FFFF);
    let mask_lo = config_read32(bus, slot, func, lo);
    let mask_hi = config_read32(bus, slot, func, hi);
    config_write32(bus, slot, func, lo, orig_lo);
    config_write32(bus, slot, func, hi, orig_hi);
    let mask = ((mask_hi as u64) << 32) | (mask_lo & !0xF) as u64;
    (!mask).wrapping_add(1)
}

fn alloc_mmio32(size: u32) -> u64 {
    let p = unsafe { PCI.get() };
    let mask = (size as u64) - 1;
    p.mmio32_next = (p.mmio32_next + mask) & !mask;
    if p.mmio32_next + size as u64 - 1 > PCI_MMIO32_LIMIT {
        uart::errorln("[PCI] 32-bit MMIO window exhausted");
        return 0;
    }
    let addr = p.mmio32_next;
    p.mmio32_next += size as u64;
    addr
}

fn alloc_mmio64(size: u64) -> u64 {
    let p = unsafe { PCI.get() };
    let mask = size - 1;
    p.mmio64_next = (p.mmio64_next + mask) & !mask;
    if p.mmio64_next + size - 1 > PCI_MMIO64_LIMIT {
        uart::errorln("[PCI] 64-bit MMIO window exhausted");
        return 0;
    }
    let addr = p.mmio64_next;
    p.mmio64_next += size;
    addr
}

/// Assign MMIO BARs and write them into the device's `bar_addr`.
pub fn assign_bars(dev: &mut PciDevice) {
    uart::println("[PCI] Assigning BARs");
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let mut i = 0u8;
    while i < 6 {
        let off = PCI_BAR0 + (i as u16) * 4;
        let bar = config_read32(b, d, f, off);
        if bar & 0x01 != 0 {
            i += 1;
            continue; // I/O BAR
        }
        let kind = (bar >> 1) & 0x03;
        if kind == 0x00 {
            let size = bar_size32(b, d, f, off);
            if size == 0 || size == 0xFFFF_FFFF {
                dev.bar_addr[i as usize] = 0;
                i += 1;
                continue;
            }
            let addr = alloc_mmio32(size);
            config_write32(b, d, f, off, addr as u32);
            dev.bar_addr[i as usize] = addr;
            kprintln!("[PCI]  BAR{} 32-bit size={:#x} -> {:#x}", i, size, addr);
        } else if kind == 0x02 {
            if i + 1 >= 6 {
                dev.bar_addr[i as usize] = 0;
                i += 1;
                continue;
            }
            let off_hi = PCI_BAR0 + ((i + 1) as u16) * 4;
            let size = bar_size64(b, d, f, off, off_hi);
            if size == 0 {
                dev.bar_addr[i as usize] = 0;
                i += 2;
                continue;
            }
            let addr = alloc_mmio64(size);
            config_write32(b, d, f, off, (addr & 0xFFFF_FFFF) as u32);
            config_write32(b, d, f, off_hi, (addr >> 32) as u32);
            dev.bar_addr[i as usize] = addr;
            kprintln!("[PCI]  BAR{} 64-bit size={:#x} -> {:#x}", i, size, addr);
            i += 2;
            continue;
        }
        i += 1;
    }
    uart::println("[PCI] BARs Assigned");
}

pub fn enable_device(dev: &PciDevice) {
    let mut cmd = config_read16(dev.bus as u16, dev.slot, dev.func, PCI_COMMAND);
    cmd |= 1 << 1; // Memory Space Enable
    cmd |= 1 << 2; // Bus Master Enable (DMA)
    config_write16(dev.bus as u16, dev.slot, dev.func, PCI_COMMAND, cmd);
    uart::println("[PCI] Device Enabled");
}

/// GIC INTID for a PCI device's legacy INTx pin on the QEMU virt machine.
/// From the board's PCIe interrupt-map: SPI = 3 + ((slot + pin-1) % 4),
/// INTID = 32 + SPI. Returns 0 if the device has no interrupt pin.
pub fn device_intid(dev: &PciDevice) -> u32 {
    let pin = config_read8(dev.bus as u16, dev.slot, dev.func, PCI_INTERRUPT_PIN);
    if pin == 0 {
        return 0;
    }
    let swizzle = (dev.slot as u32 + (pin as u32 - 1)) % 4;
    35 + swizzle
}
