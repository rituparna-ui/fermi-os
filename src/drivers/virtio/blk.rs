//! VirtIO block driver (`virtio-blk-pci`, device id 0x1042).
//!
//! Synchronous 512-byte sector read/write using a 3-descriptor chain (request
//! header + data + status byte) over a single virtqueue. Capacity is read from
//! device config at init.

use crate::drivers::pci;
use crate::drivers::virtio::virtqueue::{
    VirtqAvail, VirtqDesc, VirtqSeg, VirtqUsed, Virtqueue, VIRTQ_DESC_F_NONE, VIRTQ_DESC_F_WRITE,
    VIRTQ_MAX_SIZE,
};
use crate::drivers::virtio::*;
use crate::klib::mmio;
use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;

const VIRTIO_BLK_VENDOR_ID: u16 = 0x1AF4;
const VIRTIO_BLK_DEVICE_ID: u16 = 0x1042;
const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

pub const VIRTIO_BLK_SECTOR_SIZE: usize = 512;
const VIRTIO_BLK_CFG_CAPACITY: usize = 0x00;

const VIRTIO_BLK_T_IN: u32 = 0;
const VIRTIO_BLK_T_OUT: u32 = 1;
const VIRTIO_BLK_S_OK: u8 = 0;

/// virtio-blk request header (read by the device).
#[repr(C)]
struct BlkReq {
    type_: u32,
    reserved: u32,
    sector: u64,
}

// Page-aligned ring storage + DMA-visible request header / status byte, all in
// kernel .bss (fixed kernel VA for virt_to_phys).
#[repr(C, align(4096))]
struct BlkDesc([VirtqDesc; VIRTQ_MAX_SIZE]);
#[repr(C, align(4096))]
struct BlkAvail(VirtqAvail);
#[repr(C, align(4096))]
struct BlkUsed(VirtqUsed);
#[repr(C, align(16))]
struct BlkReqBox(BlkReq);
#[repr(C, align(16))]
struct BlkStatusBox(u8);

static mut BLK_DESC: BlkDesc = BlkDesc(
    [VirtqDesc {
        addr: 0,
        len: 0,
        flags: 0,
        next: 0,
    }; VIRTQ_MAX_SIZE],
);
static mut BLK_AVAIL: BlkAvail = BlkAvail(VirtqAvail {
    flags: 0,
    idx: 0,
    ring: [0; VIRTQ_MAX_SIZE],
});
static mut BLK_USED: BlkUsed = BlkUsed(VirtqUsed {
    flags: 0,
    idx: 0,
    ring: [crate::drivers::virtio::virtqueue::VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
});
static mut BLK_REQ: BlkReqBox = BlkReqBox(BlkReq {
    type_: 0,
    reserved: 0,
    sector: 0,
});
static mut BLK_STATUS: BlkStatusBox = BlkStatusBox(0);

struct BlkDevice {
    vq: Virtqueue,
    capacity_sectors: u64,
    ready: bool,
}

// SAFETY: rings + DMA scratch are owned by this driver and only touched under
// the lock (single-threaded init, serialized I/O afterward).
unsafe impl Send for BlkDevice {}

static BLK: SpinLock<Option<BlkDevice>> = SpinLock::new(None);

/// Discover and initialize the virtio-blk device.
pub fn init() {
    kprintln!("[BLK] Initializing Device");

    let mut dev = match pci::find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID) {
        Some(d) => d,
        None => {
            Uart.errorln("[BLK] Device not found");
            return;
        }
    };
    kprintln!("[BLK] Device found");

    if pci::get_header_type(&dev) & 0x7F != PCI_ENDPOINT_DEV_TYPE {
        Uart.errorln("[BLK]: Unexpected header type");
        return;
    }

    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);

    let mut caps = VirtioPciCaps::default();
    parse_capabilities(&dev, &mut caps);
    let base = caps.common_cfg;
    if base == 0 {
        Uart.errorln("[BLK] No common config capability");
        return;
    }

    if device_init_handshake(base, "BLK", 0).is_none() {
        return;
    }

    // SAFETY: addr_of_mut! on the ring statics owned by this driver.
    let mut vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(BLK_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(BLK_AVAIL.0) as *mut VirtqAvail,
            used: core::ptr::addr_of_mut!(BLK_USED.0) as *mut VirtqUsed,
        }
    };
    if !vq.setup(base, 0, &caps) {
        Uart.errorln("[BLK] Virtqueue setup failed");
        return;
    }

    set_driver_ok(base);
    kprintln!("[BLK] DRIVER_OK set");

    // Capacity (u64 sectors) from device config.
    let dcfg = caps.device_cfg;
    let cap_lo = mmio::read32(dcfg + VIRTIO_BLK_CFG_CAPACITY) as u64;
    let cap_hi = mmio::read32(dcfg + VIRTIO_BLK_CFG_CAPACITY + 4) as u64;
    let capacity = (cap_hi << 32) | cap_lo;
    kprintln!(
        "[BLK] Capacity: {} sectors ({} MiB)",
        capacity,
        capacity / 2048
    );

    *BLK.lock() = Some(BlkDevice {
        vq,
        capacity_sectors: capacity,
        ready: true,
    });
}

/// Number of 512-byte sectors on the device (0 if not initialized).
pub fn capacity_sectors() -> u64 {
    BLK.lock().as_ref().map_or(0, |d| d.capacity_sectors)
}

/// Issue a header+data+status descriptor chain and poll for completion.
/// `data_write` is the data segment's flag (device writes for reads, reads for
/// writes). Returns true on VIRTIO_BLK_S_OK.
fn do_request(dev: &mut BlkDevice, type_: u32, sector: u64, buf_pa: u64, data_flags: u16) -> bool {
    // SAFETY: BLK_REQ / BLK_STATUS are this driver's DMA scratch, serialized by
    // the BLK lock held by the caller.
    unsafe {
        let req = core::ptr::addr_of_mut!(BLK_REQ.0);
        (*req).type_ = type_;
        (*req).reserved = 0;
        (*req).sector = sector;
        core::ptr::addr_of_mut!(BLK_STATUS.0).write(0xFF);

        let hdr_pa = virt_to_phys(req as u64);
        let status_pa = virt_to_phys(core::ptr::addr_of!(BLK_STATUS.0) as u64);

        let segs = [
            VirtqSeg {
                pa: hdr_pa,
                len: core::mem::size_of::<BlkReq>() as u32,
                flags: VIRTQ_DESC_F_NONE,
            },
            VirtqSeg {
                pa: buf_pa,
                len: VIRTIO_BLK_SECTOR_SIZE as u32,
                flags: data_flags,
            },
            VirtqSeg {
                pa: status_pa,
                len: 1,
                flags: VIRTQ_DESC_F_WRITE,
            },
        ];
        dev.vq.submit_chain(&segs);
        dev.vq.notify();
        dev.vq.poll();

        core::ptr::addr_of!(BLK_STATUS.0).read() == VIRTIO_BLK_S_OK
    }
}

/// Read one 512-byte sector into `buf` (must be >= 512 bytes, kernel-VA).
pub fn read(sector: u64, buf: &mut [u8]) -> bool {
    if buf.len() < VIRTIO_BLK_SECTOR_SIZE {
        return false;
    }
    let mut guard = BLK.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return false,
    };
    let buf_pa = virt_to_phys(buf.as_ptr() as u64);
    let ok = do_request(dev, VIRTIO_BLK_T_IN, sector, buf_pa, VIRTQ_DESC_F_WRITE);
    if !ok {
        kprintln!("[BLK] read sector {} failed", sector);
    }
    ok
}

/// Write one 512-byte sector from `buf` (must be >= 512 bytes, kernel-VA).
pub fn write(sector: u64, buf: &[u8]) -> bool {
    if buf.len() < VIRTIO_BLK_SECTOR_SIZE {
        return false;
    }
    let mut guard = BLK.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return false,
    };
    let buf_pa = virt_to_phys(buf.as_ptr() as u64);
    let ok = do_request(dev, VIRTIO_BLK_T_OUT, sector, buf_pa, VIRTQ_DESC_F_NONE);
    if !ok {
        kprintln!("[BLK] write sector {} failed", sector);
    }
    ok
}
