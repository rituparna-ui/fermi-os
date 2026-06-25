//! VirtIO RNG driver (`virtio-rng-pci`, device id 0x1044).
//!
//! Runs the full VirtIO init sequence (reset → ack → driver → feature
//! negotiation → FEATURES_OK → queue setup → DRIVER_OK), then serves entropy
//! via a single read-only virtqueue and a DMA bounce buffer.

use crate::arch::cpu::dsb_sy;
use crate::drivers::pci;
use crate::drivers::virtio::virtqueue::{Virtqueue, VirtqAvail, VirtqDesc, VirtqUsed, VIRTQ_DESC_F_WRITE, VIRTQ_MAX_SIZE};
use crate::drivers::virtio::*;
use crate::klib::mmio;
use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;

const VIRTIO_RNG_VENDOR_ID: u16 = 0x1AF4;
const VIRTIO_RNG_DEVICE_ID: u16 = 0x1044;
const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

const RNG_BUF_SIZE: usize = 256;

// Page-aligned ring + bounce-buffer storage in kernel .bss (fixed kernel VA, so
// virt_to_phys is meaningful for DMA). Accessed only under RNG_READY + the
// driver's single-threaded init / serialized read path.
#[repr(C, align(4096))]
struct RngDesc([VirtqDesc; VIRTQ_MAX_SIZE]);
#[repr(C, align(4096))]
struct RngAvail(VirtqAvail);
#[repr(C, align(4096))]
struct RngUsed(VirtqUsed);
#[repr(C, align(64))]
struct RngBuf([u8; RNG_BUF_SIZE]);

static mut RNG_DESC: RngDesc = RngDesc(
    [VirtqDesc {
        addr: 0,
        len: 0,
        flags: 0,
        next: 0,
    }; VIRTQ_MAX_SIZE],
);
static mut RNG_AVAIL: RngAvail = RngAvail(VirtqAvail {
    flags: 0,
    idx: 0,
    ring: [0; VIRTQ_MAX_SIZE],
});
static mut RNG_USED: RngUsed = RngUsed(VirtqUsed {
    flags: 0,
    idx: 0,
    ring: [crate::drivers::virtio::virtqueue::VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
});
static mut RNG_BUF: RngBuf = RngBuf([0; RNG_BUF_SIZE]);

struct RngDevice {
    caps: VirtioPciCaps,
    vq: Virtqueue,
    ready: bool,
}

// SAFETY: the raw ring pointers in `vq` are only used under the lock, and the
// driver runs single-threaded during boot and serialized via the lock after.
unsafe impl Send for RngDevice {}

static RNG: SpinLock<Option<RngDevice>> = SpinLock::new(None);

/// Read `count` random bytes into `dst`. Returns the number of bytes produced.
pub fn read(dst: &mut [u8]) -> usize {
    let mut guard = RNG.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return 0,
    };

    let count = dst.len();
    let mut done = 0;
    while done < count {
        let chunk = core::cmp::min(count - done, RNG_BUF_SIZE);
        let pa = virt_to_phys(core::ptr::addr_of!(RNG_BUF) as u64);
        dev.vq.submit(pa, chunk as u32, VIRTQ_DESC_F_WRITE);
        dev.vq.notify();
        dev.vq.poll();
        // Copy out of the bounce buffer.
        unsafe {
            let buf = core::ptr::addr_of!(RNG_BUF.0) as *const u8;
            for i in 0..chunk {
                dst[done + i] = buf.add(i).read();
            }
        }
        done += chunk;
    }
    done
}

/// Discover and initialize the virtio-rng device.
pub fn init() {
    kprintln!("[RNG] Initializing Device");

    let mut dev = match pci::find_device(VIRTIO_RNG_VENDOR_ID, VIRTIO_RNG_DEVICE_ID) {
        Some(d) => d,
        None => {
            Uart.errorln("[RNG] Device not found");
            return;
        }
    };
    kprintln!("[RNG] Device found");

    if pci::get_header_type(&dev) & 0x7F != PCI_ENDPOINT_DEV_TYPE {
        Uart.errorln("[RNG]: Unexpected header type");
        return;
    }

    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);

    let mut caps = VirtioPciCaps::default();
    parse_capabilities(&dev, &mut caps);

    let base = caps.common_cfg;
    if base == 0 {
        Uart.errorln("[RNG] No common config capability");
        return;
    }

    if !virtio_init_sequence(base) {
        return;
    }

    // Set up virtqueue 0 over the static ring storage.
    // SAFETY: addr_of_mut! on the ring statics; the rings are owned exclusively
    // by this driver (single-threaded init, lock-serialized reads afterward).
    let mut vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(RNG_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(RNG_AVAIL.0) as *mut VirtqAvail,
            used: core::ptr::addr_of_mut!(RNG_USED.0) as *mut VirtqUsed,
        }
    };
    if !vq.setup(base, 0, &caps) {
        Uart.errorln("[RNG] Virtqueue setup failed");
        return;
    }

    // DRIVER_OK.
    let status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    dsb_sy();
    kprintln!("[RNG] DRIVER_OK set");

    *RNG.lock() = Some(RngDevice {
        caps,
        vq,
        ready: true,
    });
}

/// Run the standard VirtIO device init handshake on the common-cfg at `base`,
/// negotiating only VIRTIO_F_VERSION_1. Returns false on failure.
/// Shared shape with all VirtIO drivers; the RNG has no device-specific
/// features.
fn virtio_init_sequence(base: usize) -> bool {
    // Reset.
    kprintln!("[RNG][VIRTIO-INIT][1] Reset Device");
    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}
    kprintln!("[RNG][VIRTIO-INIT][1] Reset Device Complete");

    // ACK.
    kprintln!("[RNG][VIRTIO-INIT][2] Ack");
    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
    dsb_sy();

    // DRIVER.
    kprintln!("[RNG][VIRTIO-INIT][3] Driver Status");
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    // Feature negotiation.
    kprintln!("[RNG][VIRTIO-INIT][4] Negotiate Features");
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 0);
    dsb_sy();
    let feat_lo = mmio::read32(base + VIRTIO_COMMON_DF);
    kprintln!(" Device features[0]: {:#x}", feat_lo);
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 1);
    dsb_sy();
    let feat_hi = mmio::read32(base + VIRTIO_COMMON_DF);
    kprintln!(" Device features[1]: {:#x}", feat_hi);

    // Accept only VIRTIO_F_VERSION_1 (bit 32 == feat_hi bit 0).
    let guest_lo = 0u32;
    let guest_hi = feat_hi & 0x01;
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_lo);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 1);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_hi);
    dsb_sy();
    kprintln!(" Accepted Features: lo={:#x} hi={:#x}", guest_lo, guest_hi);

    // FEATURES_OK + verify.
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    if status & VIRTIO_STATUS_FEATURES_OK == 0 {
        Uart.errorln("[RNG] FEATURES_OK failed");
        return false;
    }
    kprintln!("[RNG] Status: {:#x}", status);
    kprintln!("[RNG] FEATURES_OK !");
    true
}
