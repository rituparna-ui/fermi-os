//! VirtIO balloon driver (`virtio-balloon-pci`, device id 0x1045).
//!
//! Cooperative memory ballooning: inflate hands PMM pages to the host (reported
//! as PFNs on the inflate queue), deflate reclaims them. Driver-initiated (no
//! config-change IRQ wired); the shell exposes inflate/deflate and /proc shows
//! actual vs the host's target.

use crate::drivers::pci;
use crate::drivers::virtio::virtqueue::{
    VirtqAvail, VirtqDesc, VirtqUsed, VirtqUsedElem, Virtqueue, VIRTQ_DESC_F_NONE, VIRTQ_MAX_SIZE,
};
use crate::drivers::virtio::*;
use crate::klib::mmio;
use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;
use crate::mm::pmm;

const VIRTIO_BALLOON_VENDOR_ID: u16 = 0x1AF4;
const VIRTIO_BALLOON_DEVICE_ID: u16 = 0x1045;
const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

const PFN_SHIFT: u64 = 12;
const MAX_PAGES: usize = 1024;
const VQ_INFLATE: u16 = 0;
const VQ_DEFLATE: u16 = 1;
const CFG_NUM_PAGES: usize = 0x00;
const CFG_ACTUAL: usize = 0x04;

#[repr(C, align(4096))]
struct RingDesc([VirtqDesc; VIRTQ_MAX_SIZE]);
#[repr(C, align(4096))]
struct RingAvail(VirtqAvail);
#[repr(C, align(4096))]
struct RingUsed(VirtqUsed);
#[repr(C, align(4096))]
struct PfnBuf([u32; MAX_PAGES]);

const DESC_INIT: RingDesc = RingDesc(
    [VirtqDesc {
        addr: 0,
        len: 0,
        flags: 0,
        next: 0,
    }; VIRTQ_MAX_SIZE],
);
const AVAIL_INIT: RingAvail = RingAvail(VirtqAvail {
    flags: 0,
    idx: 0,
    ring: [0; VIRTQ_MAX_SIZE],
});
const USED_INIT: RingUsed = RingUsed(VirtqUsed {
    flags: 0,
    idx: 0,
    ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
});

static mut INF_DESC: RingDesc = DESC_INIT;
static mut INF_AVAIL: RingAvail = AVAIL_INIT;
static mut INF_USED: RingUsed = USED_INIT;
static mut DEF_DESC: RingDesc = DESC_INIT;
static mut DEF_AVAIL: RingAvail = AVAIL_INIT;
static mut DEF_USED: RingUsed = USED_INIT;
/// DMA buffer the device reads PFNs from.
static mut PFN_BUF: PfnBuf = PfnBuf([0; MAX_PAGES]);

struct BalloonDevice {
    inflate_vq: Virtqueue,
    deflate_vq: Virtqueue,
    device_cfg: usize,
    actual: u32,
    /// PFNs currently given to the host (active prefix [0..actual)).
    inflated: [u32; MAX_PAGES],
    ready: bool,
}

// SAFETY: rings + PFN buffer owned by this driver, accessed only under the lock.
unsafe impl Send for BalloonDevice {}

static BALLOON: SpinLock<Option<BalloonDevice>> = SpinLock::new(None);

/// Submit a device-readable PFN batch on `vq` and poll for the device ack.
fn submit_pfn_batch(vq: &mut Virtqueue, count: u32) {
    if count == 0 {
        return;
    }
    // SAFETY: PFN_BUF is this driver's DMA scratch, serialized by the lock.
    let pa = unsafe { virt_to_phys(core::ptr::addr_of!(PFN_BUF.0) as u64) };
    vq.submit(pa, count * core::mem::size_of::<u32>() as u32, VIRTQ_DESC_F_NONE);
    vq.notify();
    vq.poll();
}

fn publish_actual(dev: &BalloonDevice) {
    if dev.device_cfg == 0 {
        return;
    }
    mmio::write32(dev.device_cfg + CFG_ACTUAL, dev.actual);
    crate::arch::cpu::dsb_sy();
}

/// Hand `n` PMM pages to the host. Returns the number actually inflated.
pub fn inflate(n: u32) -> i64 {
    let mut guard = BALLOON.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return -1,
    };

    let headroom = MAX_PAGES as u32 - dev.actual;
    let n = core::cmp::min(n, headroom);
    if n == 0 {
        return 0;
    }

    // Allocate fresh pages; stage their PFNs into PFN_BUF and the tracking array.
    let mut got = 0u32;
    unsafe {
        let buf = core::ptr::addr_of_mut!(PFN_BUF.0) as *mut u32;
        for _ in 0..n {
            let pa = pmm::allocate_page();
            if pa == 0 {
                break;
            }
            let pfn = (pa >> PFN_SHIFT) as u32;
            buf.add(got as usize).write(pfn);
            dev.inflated[(dev.actual + got) as usize] = pfn;
            got += 1;
        }
    }
    if got == 0 {
        return 0;
    }

    submit_pfn_batch(&mut dev.inflate_vq, got);
    dev.actual += got;
    publish_actual(dev);
    got as i64
}

/// Reclaim `n` pages from the host back into the PMM. Returns number deflated.
pub fn deflate(n: u32) -> i64 {
    let mut guard = BALLOON.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return -1,
    };

    let n = core::cmp::min(n, dev.actual);
    if n == 0 {
        return 0;
    }

    // Pop most-recently inflated PFNs (LIFO) into PFN_BUF.
    let base = dev.actual - n;
    unsafe {
        let buf = core::ptr::addr_of_mut!(PFN_BUF.0) as *mut u32;
        for i in 0..n {
            buf.add(i as usize).write(dev.inflated[(base + i) as usize]);
        }
    }

    submit_pfn_batch(&mut dev.deflate_vq, n);

    // Return the physical pages to the PMM.
    unsafe {
        let buf = core::ptr::addr_of!(PFN_BUF.0) as *const u32;
        for i in 0..n {
            let pfn = buf.add(i as usize).read() as u64;
            pmm::free_page(pfn << PFN_SHIFT);
        }
    }

    dev.actual -= n;
    publish_actual(dev);
    n as i64
}

/// (actual_pages, host_target) snapshot for /proc and the shell.
pub fn status() -> (u32, u32) {
    let guard = BALLOON.lock();
    match guard.as_ref() {
        Some(d) if d.ready => {
            let target = if d.device_cfg == 0 {
                0
            } else {
                mmio::read32(d.device_cfg + CFG_NUM_PAGES)
            };
            (d.actual, target)
        }
        _ => (0, 0),
    }
}

/// Discover and initialize the virtio-balloon device.
pub fn init() {
    kprintln!("[BALLOON] Initializing Device");

    let mut dev = match pci::find_device(VIRTIO_BALLOON_VENDOR_ID, VIRTIO_BALLOON_DEVICE_ID) {
        Some(d) => d,
        None => {
            kprintln!("[BALLOON] Device not found (skipping)");
            return;
        }
    };
    kprintln!("[BALLOON] Device found");

    if pci::get_header_type(&dev) & 0x7F != PCI_ENDPOINT_DEV_TYPE {
        Uart.errorln("[BALLOON] Unexpected header type");
        return;
    }

    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);
    let mut caps = VirtioPciCaps::default();
    parse_capabilities(&dev, &mut caps);
    let base = caps.common_cfg;
    if base == 0 {
        Uart.errorln("[BALLOON] No common config capability");
        return;
    }

    if device_init_handshake(base, "BALLOON", 0).is_none() {
        return;
    }

    // SAFETY: ring statics owned by this driver.
    let mut inflate_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(INF_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(INF_AVAIL.0),
            used: core::ptr::addr_of_mut!(INF_USED.0),
        }
    };
    if !inflate_vq.setup(base, VQ_INFLATE, &caps) {
        Uart.errorln("[BALLOON] inflateq setup failed");
        return;
    }

    let mut deflate_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(DEF_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(DEF_AVAIL.0),
            used: core::ptr::addr_of_mut!(DEF_USED.0),
        }
    };
    if !deflate_vq.setup(base, VQ_DEFLATE, &caps) {
        Uart.errorln("[BALLOON] deflateq setup failed");
        return;
    }

    set_driver_ok(base);

    let mut device = BalloonDevice {
        inflate_vq,
        deflate_vq,
        device_cfg: caps.device_cfg,
        actual: 0,
        inflated: [0; MAX_PAGES],
        ready: true,
    };
    publish_actual(&device);
    let target = if caps.device_cfg != 0 {
        mmio::read32(caps.device_cfg + CFG_NUM_PAGES)
    } else {
        0
    };
    kprintln!("[BALLOON] DRIVER_OK; host target={} pages, actual=0", target);
    device.ready = true;
    *BALLOON.lock() = Some(device);
}
