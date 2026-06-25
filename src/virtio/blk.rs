//! VirtIO block driver — synchronous 512-byte sector read/write.
//!
//! Port of `src/pci/virtio/blk/blk.c`.

use super::virtqueue::{self, Virtqueue, VirtqAvail, VirtqDesc, VirtqSeg, VirtqUsed, VirtqUsedElem, VIRTQ_DESC_F_WRITE, VIRTQ_MAX_SIZE};
use super::*;
use crate::kprintln;
use crate::mm::mmu::virt_to_phys;
use crate::mmio;
use crate::pci;
use crate::sync::{Racy, SpinLock};
use crate::uart;

pub const VIRTIO_BLK_VENDOR_ID: u16 = 0x1AF4;
pub const VIRTIO_BLK_DEVICE_ID: u16 = 0x1042;
pub const VIRTIO_BLK_SECTOR_SIZE: usize = 512;

const VIRTIO_BLK_CFG_CAPACITY: usize = 0x00;
const VIRTIO_BLK_T_IN: u32 = 0;
const VIRTIO_BLK_T_OUT: u32 = 1;
const VIRTIO_BLK_S_OK: u8 = 0;

#[repr(C, align(16))]
struct BlkReq {
    type_: u32,
    reserved: u32,
    sector: u64,
}

#[repr(C, align(4096))]
struct BlkBacking {
    desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    avail: VirtqAvail,
    used: VirtqUsed,
    hdr: BlkReq,
    status: u8,
    _pad: [u8; 15],
}

static BACKING: Racy<BlkBacking> = Racy::new(BlkBacking {
    desc: [VirtqDesc { addr: 0, len: 0, flags: 0, next: 0 }; VIRTQ_MAX_SIZE],
    avail: VirtqAvail { flags: 0, idx: 0, ring: [0; VIRTQ_MAX_SIZE] },
    used: VirtqUsed { flags: 0, idx: 0, ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE] },
    hdr: BlkReq { type_: 0, reserved: 0, sector: 0 },
    status: 0,
    _pad: [0; 15],
});

struct BlkDev {
    caps: VirtioPciCaps,
    vq: Virtqueue,
    capacity_sectors: u64,
    ready: bool,
}

static BLK: Racy<BlkDev> = Racy::new(BlkDev {
    caps: VirtioPciCaps { common_cfg: 0, notify_base: 0, isr_cfg: 0, device_cfg: 0, notify_off_multiplier: 0 },
    vq: Virtqueue::empty(),
    capacity_sectors: 0,
    ready: false,
});

pub fn init() {
    uart::println("[BLK] Initializing Device");
    let mut dev = match pci::find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID) {
        Some(d) => d,
        None => {
            uart::errorln("[BLK] Device not found");
            return;
        }
    };
    if pci::header_type(&dev) & 0x7F != pci::PCI_ENDPOINT_DEV_TYPE {
        uart::errorln("[BLK] Unexpected header type");
        return;
    }
    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);

    let blk = unsafe { BLK.get() };
    super::parse_capabilities(&dev, &mut blk.caps);
    let base = blk.caps.common_cfg;

    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}
    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 1);
    dsb_sy();
    let feat_hi = mmio::read32(base + VIRTIO_COMMON_DF);
    let guest_hi = feat_hi & 0x01;
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 1);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_hi);
    dsb_sy();

    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    if status & VIRTIO_STATUS_FEATURES_OK == 0 {
        uart::errorln("[BLK] FEATURES_OK failed");
        return;
    }

    let backing = unsafe { BACKING.get() };
    blk.vq.desc = backing.desc.as_mut_ptr();
    blk.vq.avail = &mut backing.avail as *mut VirtqAvail;
    blk.vq.used = &mut backing.used as *mut VirtqUsed;
    if !virtqueue::setup(base, 0, &mut blk.vq, &blk.caps) {
        uart::errorln("[BLK] Virtqueue setup failed");
        return;
    }

    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    dsb_sy();

    let dcfg = blk.caps.device_cfg;
    let cap_lo = mmio::read32(dcfg + VIRTIO_BLK_CFG_CAPACITY) as u64;
    let cap_hi = mmio::read32(dcfg + VIRTIO_BLK_CFG_CAPACITY + 4) as u64;
    blk.capacity_sectors = (cap_hi << 32) | cap_lo;
    blk.ready = true;
    kprintln!(
        "[BLK] DRIVER_OK; capacity {} sectors ({} MiB)",
        blk.capacity_sectors,
        blk.capacity_sectors / 2048
    );
}

pub fn capacity_sectors() -> u64 {
    unsafe { BLK.get() }.capacity_sectors
}

pub fn is_ready() -> bool {
    unsafe { BLK.get() }.ready
}

/// Serializes block requests: there is a single shared request header and
/// virtqueue, so concurrent callers (e.g. boot ELF loads racing shell FS ops)
/// would corrupt it. Busy-poll completion means a plain spinlock is safe.
static BLK_LOCK: SpinLock<()> = SpinLock::new(());

fn rw(sector: u64, buf_pa: u64, write: bool) -> bool {
    let _guard = BLK_LOCK.lock();
    let blk = unsafe { BLK.get() };
    if !blk.ready {
        return false;
    }
    let backing = unsafe { BACKING.get() };
    backing.hdr.type_ = if write { VIRTIO_BLK_T_OUT } else { VIRTIO_BLK_T_IN };
    backing.hdr.reserved = 0;
    backing.hdr.sector = sector;
    backing.status = 0xFF;

    let hdr_pa = virt_to_phys(&backing.hdr as *const BlkReq as u64);
    let status_pa = virt_to_phys(&backing.status as *const u8 as u64);
    let data_flags = if write { 0 } else { VIRTQ_DESC_F_WRITE };
    let segs = [
        VirtqSeg { pa: hdr_pa, len: core::mem::size_of::<BlkReq>() as u32, flags: 0 },
        VirtqSeg { pa: buf_pa, len: VIRTIO_BLK_SECTOR_SIZE as u32, flags: data_flags },
        VirtqSeg { pa: status_pa, len: 1, flags: VIRTQ_DESC_F_WRITE },
    ];
    virtqueue::submit_chain(&mut blk.vq, &segs);
    virtqueue::notify(&blk.vq);
    virtqueue::poll(&mut blk.vq);

    if backing.status != VIRTIO_BLK_S_OK {
        kprintln!("[BLK] sector {} io failed status={:#x}", sector, backing.status);
        return false;
    }
    true
}

/// Read one 512-byte sector into `buf` (must be >= 512 bytes, kernel-mapped).
pub fn read(sector: u64, buf: &mut [u8]) -> bool {
    let pa = virt_to_phys(buf.as_ptr() as u64);
    let ok = rw(sector, pa, false);
    ok
}

/// Write one 512-byte sector from `buf`.
pub fn write(sector: u64, buf: &[u8]) -> bool {
    let pa = virt_to_phys(buf.as_ptr() as u64);
    rw(sector, pa, true)
}
