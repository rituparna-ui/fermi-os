//! PCI Express (ECAM) bus enumeration + BAR assignment for the QEMU `virt`
//! machine.
//!
//! QEMU virt PCI layout:
//!   ECAM base:    0x40_1000_0000 (256 buses)
//!   32-bit MMIO:  0x1000_0000 .. 0x3EFE_FFFF
//!   64-bit MMIO:  0x80_0000_0000 .. 0xFF_FFFF_FFFF
//!
//! Brute-force enumerates all bus/slot/func, caches discovered devices, and
//! assigns/sizes 32- and 64-bit memory BARs by the write-all-ones probe.

use crate::klib::mmio;
use crate::klib::sync::SpinLock;
use crate::kprintln;

const PCI_ECAM_PHYS: usize = 0x40_1000_0000;
const PCI_MMIO32_PHYS: u64 = 0x1000_0000;
const PCI_MMIO32_LIMIT: u64 = 0x3EFE_FFFF;
const PCI_MMIO64_PHYS: u64 = 0x80_0000_0000;
const PCI_MMIO64_LIMIT: u64 = 0xFF_FFFF_FFFF;

const MAX_PCI_DEVICES: usize = 16;
const MAX_PCI_BUS: u16 = 256;
const MAX_PCI_SLOT: u8 = 32;
const MAX_PCI_FUNC: u8 = 8;

// Config space offsets.
const PCI_VENDOR_ID: u16 = 0x00;
const PCI_DEVICE_ID: u16 = 0x02;
const PCI_COMMAND: u16 = 0x04;
const PCI_HEADER_TYPE: u16 = 0x0E;
const PCI_BAR0: u16 = 0x10;
pub const PCI_CAP_PTR: u16 = 0x34;

/// A discovered PCI device with assigned BAR addresses.
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
    count: usize,
    mmio32_next: u64,
    mmio64_next: u64,
}

static PCI: SpinLock<PciState> = SpinLock::new(PciState {
    devices: [PciDevice::empty(); MAX_PCI_DEVICES],
    count: 0,
    mmio32_next: PCI_MMIO32_PHYS,
    mmio64_next: PCI_MMIO64_PHYS,
});

// --- ECAM config-space accessors --------------------------------------------
//
// ECAM addresses are physical; the MMIO layer adds KERNEL_VA_OFFSET (after
// switch_to_upper) so they resolve through TTBR1.

fn ecam_addr(bus: u16, slot: u8, func: u8, offset: u16) -> usize {
    PCI_ECAM_PHYS
        | ((bus as usize) << 20)
        | ((slot as usize) << 15)
        | ((func as usize) << 12)
        | (offset as usize)
}

pub fn config_read32(bus: u16, slot: u8, func: u8, offset: u16) -> u32 {
    mmio::read32(ecam_addr(bus, slot, func, offset))
}
pub fn config_read16(bus: u16, slot: u8, func: u8, offset: u16) -> u16 {
    mmio::read16(ecam_addr(bus, slot, func, offset))
}
pub fn config_read8(bus: u16, slot: u8, func: u8, offset: u16) -> u8 {
    mmio::read8(ecam_addr(bus, slot, func, offset))
}
pub fn config_write32(bus: u16, slot: u8, func: u8, offset: u16, val: u32) {
    mmio::write32(ecam_addr(bus, slot, func, offset), val);
}
pub fn config_write16(bus: u16, slot: u8, func: u8, offset: u16, val: u16) {
    mmio::write16(ecam_addr(bus, slot, func, offset), val);
}
pub fn config_write8(bus: u16, slot: u8, func: u8, offset: u16, val: u8) {
    mmio::write8(ecam_addr(bus, slot, func, offset), val);
}

/// Brute-force enumerate every bus/slot/func and cache present devices.
pub fn enumerate_bus() {
    kprintln!("[PCI] Enumerating PCI Devices");
    let mut pci = PCI.lock();

    for bus in 0..MAX_PCI_BUS {
        for slot in 0..MAX_PCI_SLOT {
            for func in 0..MAX_PCI_FUNC {
                let vendor_id = config_read16(bus, slot, func, PCI_VENDOR_ID);
                if vendor_id == 0xFFFF {
                    continue;
                }
                let device_id = config_read16(bus, slot, func, PCI_DEVICE_ID);
                kprintln!(
                    "[PCI] Device found at {}:{}.{} | VendorID: {:#x}, DeviceID: {:#x}",
                    bus,
                    slot,
                    func,
                    vendor_id,
                    device_id
                );
                if pci.count >= MAX_PCI_DEVICES {
                    crate::klib::uart::Uart.errorln("[PCI] Max PCI devices limit reached");
                    return;
                }
                let idx = pci.count;
                pci.devices[idx] = PciDevice {
                    bus: bus as u8,
                    slot,
                    func,
                    vendor_id,
                    device_id,
                    bar_addr: [0; 6],
                };
                pci.count += 1;
            }
        }
    }
}

/// Find a cached device by vendor/device id, returning a copy.
pub fn find_device(vendor_id: u16, device_id: u16) -> Option<PciDevice> {
    let pci = PCI.lock();
    pci.devices[..pci.count]
        .iter()
        .find(|d| d.vendor_id == vendor_id && d.device_id == device_id)
        .copied()
}

pub fn get_header_type(dev: &PciDevice) -> u8 {
    config_read8(dev.bus as u16, dev.slot, dev.func, PCI_HEADER_TYPE)
}

fn bar_size32(bus: u16, slot: u8, func: u8, offset: u16) -> u32 {
    let original = config_read32(bus, slot, func, offset);
    config_write32(bus, slot, func, offset, 0xFFFF_FFFF);
    let mut size_mask = config_read32(bus, slot, func, offset);
    config_write32(bus, slot, func, offset, original);
    size_mask &= !0xF;
    if size_mask == 0 {
        return 0;
    }
    (!size_mask).wrapping_add(1)
}

fn bar_size64(bus: u16, slot: u8, func: u8, off_lo: u16, off_hi: u16) -> u64 {
    let orig_lo = config_read32(bus, slot, func, off_lo);
    let orig_hi = config_read32(bus, slot, func, off_hi);
    config_write32(bus, slot, func, off_lo, 0xFFFF_FFFF);
    config_write32(bus, slot, func, off_hi, 0xFFFF_FFFF);
    let mask_lo = config_read32(bus, slot, func, off_lo);
    let mask_hi = config_read32(bus, slot, func, off_hi);
    config_write32(bus, slot, func, off_lo, orig_lo);
    config_write32(bus, slot, func, off_hi, orig_hi);
    let mask = ((mask_hi as u64) << 32) | ((mask_lo & !0xF) as u64);
    (!mask).wrapping_add(1)
}

fn alloc_mmio32(pci: &mut PciState, size: u32) -> u64 {
    let mask = size as u64 - 1;
    pci.mmio32_next = (pci.mmio32_next + mask) & !mask;
    if pci.mmio32_next + size as u64 - 1 > PCI_MMIO32_LIMIT {
        crate::klib::uart::Uart.errorln("[PCI] 32-bit MMIO window exhausted");
        return 0;
    }
    let addr = pci.mmio32_next;
    pci.mmio32_next += size as u64;
    addr
}

fn alloc_mmio64(pci: &mut PciState, size: u64) -> u64 {
    let mask = size - 1;
    pci.mmio64_next = (pci.mmio64_next + mask) & !mask;
    if pci.mmio64_next + size - 1 > PCI_MMIO64_LIMIT {
        crate::klib::uart::Uart.errorln("[PCI] 64-bit MMIO window exhausted");
        return 0;
    }
    let addr = pci.mmio64_next;
    pci.mmio64_next += size;
    addr
}

/// Size and assign all memory BARs of `dev`, writing the addresses back to the
/// device's `bar_addr` array (and into the cached copy).
pub fn assign_bars(dev: &mut PciDevice) {
    kprintln!("[PCI] Assigning BARs");
    let (b, d, f) = (dev.bus as u16, dev.slot, dev.func);
    let mut pci = PCI.lock();

    let mut i = 0usize;
    while i < 6 {
        let bar_offset = PCI_BAR0 + (i as u16) * 4;
        let bar = config_read32(b, d, f, bar_offset);

        if bar & 0x01 != 0 {
            crate::klib::uart::Uart.errorln("[PCI] IO BAR Type, Ignoring");
            i += 1;
            continue;
        }

        let bar_type = (bar >> 1) & 0x03;
        if bar_type == 0x00 {
            // 32-bit BAR.
            let size = bar_size32(b, d, f, bar_offset);
            if size == 0 || size == 0xFFFF_FFFF {
                dev.bar_addr[i] = 0;
                i += 1;
                continue;
            }
            kprintln!("[PCI][32 Bit Memory Space] BAR{} has size: {:#x}", i, size);
            let addr = alloc_mmio32(&mut pci, size);
            config_write32(b, d, f, bar_offset, addr as u32);
            dev.bar_addr[i] = addr;
            i += 1;
        } else if bar_type == 0x02 {
            // 64-bit BAR (occupies BAR[i] and BAR[i+1]).
            if i + 1 >= 6 {
                crate::klib::uart::Uart
                    .errorln("[PCI] 64-bit BAR cannot occupy BAR5 (no upper half)");
                dev.bar_addr[i] = 0;
                i += 1;
                continue;
            }
            let bar_offset_hi = PCI_BAR0 + ((i + 1) as u16) * 4;
            let size64 = bar_size64(b, d, f, bar_offset, bar_offset_hi);
            if size64 == 0 {
                dev.bar_addr[i] = 0;
                i += 2;
                continue;
            }
            kprintln!(
                "[PCI][64 Bit Memory Space] BAR{} has size: {:#x}",
                i,
                size64
            );
            let addr = alloc_mmio64(&mut pci, size64);
            config_write32(b, d, f, bar_offset, (addr & 0xFFFF_FFFF) as u32);
            config_write32(b, d, f, bar_offset + 4, (addr >> 32) as u32);
            dev.bar_addr[i] = addr;
            i += 2;
        } else {
            crate::klib::uart::Uart.errorln("[PCI][Memory Space Type] Huh ?");
            i += 1;
        }
    }
    drop(pci);
    kprintln!("[PCI] BARs Assigned");
}

/// Enable memory-space decoding + bus-master (DMA) for `dev`.
pub fn enable_device(dev: &PciDevice) {
    kprintln!("[PCI] Enabling device");
    let mut cmd = config_read16(dev.bus as u16, dev.slot, dev.func, PCI_COMMAND);
    cmd |= 1 << 1; // Memory Space Enable
    cmd |= 1 << 2; // Bus Master Enable (DMA)
    config_write16(dev.bus as u16, dev.slot, dev.func, PCI_COMMAND, cmd);
    kprintln!("[PCI] Device Enabled");
}
