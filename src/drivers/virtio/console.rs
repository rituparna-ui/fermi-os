//! VirtIO console driver (`virtio-console`, device id 0x1043), TX-only path.
//!
//! QEMU wires the backend to a host chardev; bytes pushed down the TX
//! virtqueue land there — a logging side-channel separate from the PL011 UART.
//! Both queues are configured (the spec requires every advertised queue be set
//! up before DRIVER_OK) but only TX is used; RX buffers are never posted.

use crate::drivers::pci;
use crate::drivers::virtio::virtqueue::{
    VirtqAvail, VirtqDesc, VirtqUsed, VirtqUsedElem, Virtqueue, VIRTQ_DESC_F_NONE, VIRTQ_MAX_SIZE,
};
use crate::drivers::virtio::*;
use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;

const VIRTIO_CONSOLE_VENDOR_ID: u16 = 0x1AF4;
const VIRTIO_CONSOLE_DEVICE_ID: u16 = 0x1043;
const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

const VQ_RX: u16 = 0;
const VQ_TX: u16 = 1;

const CONSOLE_TX_BUF: usize = 4096;

// Page-aligned ring storage (matching the other drivers): guarantees the
// descriptor table's 16-byte alignment requirement and keeps each ring on its
// own page. One macro instance per queue.
#[repr(C, align(4096))]
struct RingDesc([VirtqDesc; VIRTQ_MAX_SIZE]);
#[repr(C, align(4096))]
struct RingAvail(VirtqAvail);
#[repr(C, align(4096))]
struct RingUsed(VirtqUsed);
#[repr(align(64))]
struct Align64<T>(T);

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

static mut RX_DESC: RingDesc = DESC_INIT;
static mut RX_AVAIL: RingAvail = AVAIL_INIT;
static mut RX_USED: RingUsed = USED_INIT;
static mut TX_DESC: RingDesc = DESC_INIT;
static mut TX_AVAIL: RingAvail = AVAIL_INIT;
static mut TX_USED: RingUsed = USED_INIT;
static mut TX_BUF: Align64<[u8; CONSOLE_TX_BUF]> = Align64([0; CONSOLE_TX_BUF]);

struct ConsoleDevice {
    tx_vq: Virtqueue,
    ready: bool,
}

// SAFETY: rings + TX buffer owned by this driver, accessed only under the lock.
unsafe impl Send for ConsoleDevice {}

static CONSOLE: SpinLock<Option<ConsoleDevice>> = SpinLock::new(None);

/// Push `buf` to the host via the TX virtqueue. Returns bytes accepted, or -1
/// if the driver isn't ready.
pub fn send(buf: &[u8]) -> i64 {
    let mut guard = CONSOLE.lock();
    let dev = match guard.as_mut() {
        Some(d) if d.ready => d,
        _ => return -1,
    };
    if buf.is_empty() {
        return 0;
    }

    let mut done = 0usize;
    while done < buf.len() {
        let chunk = core::cmp::min(buf.len() - done, CONSOLE_TX_BUF);
        // SAFETY: TX_BUF is this driver's DMA staging, serialized by the lock.
        unsafe {
            let dst = core::ptr::addr_of_mut!(TX_BUF.0) as *mut u8;
            core::ptr::copy_nonoverlapping(buf.as_ptr().add(done), dst, chunk);
            let pa = virt_to_phys(dst as u64);
            // virtio-console TX has no header: raw device-readable bytes.
            dev.tx_vq.submit(pa, chunk as u32, VIRTQ_DESC_F_NONE);
            dev.tx_vq.notify();
            dev.tx_vq.poll();
        }
        done += chunk;
    }
    done as i64
}

/// Discover and initialize the virtio-console device (TX-only).
pub fn init() {
    kprintln!("[CONSOLE] Initializing Device");

    let mut dev = match pci::find_device(VIRTIO_CONSOLE_VENDOR_ID, VIRTIO_CONSOLE_DEVICE_ID) {
        Some(d) => d,
        None => {
            kprintln!("[CONSOLE] Device not found (skipping)");
            return;
        }
    };
    kprintln!("[CONSOLE] Device found");

    if pci::get_header_type(&dev) & 0x7F != PCI_ENDPOINT_DEV_TYPE {
        Uart.errorln("[CONSOLE] Unexpected header type");
        return;
    }

    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);
    let mut caps = VirtioPciCaps::default();
    parse_capabilities(&dev, &mut caps);
    let base = caps.common_cfg;
    if base == 0 {
        Uart.errorln("[CONSOLE] No common config capability");
        return;
    }

    // Accept only VIRTIO_F_VERSION_1; reject all console-specific features
    // (SIZE / MULTIPORT / EMERG_WRITE).
    if device_init_handshake(base, "CONSOLE", 0).is_none() {
        return;
    }

    // Configure both queues (spec requires every advertised queue be set up
    // before DRIVER_OK); only TX is actually used.
    // SAFETY: ring statics owned by this driver.
    let mut rx_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(RX_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(RX_AVAIL.0),
            used: core::ptr::addr_of_mut!(RX_USED.0),
        }
    };
    if !rx_vq.setup(base, VQ_RX, &caps) {
        Uart.errorln("[CONSOLE] rx queue setup failed");
        return;
    }

    let mut tx_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(TX_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(TX_AVAIL.0),
            used: core::ptr::addr_of_mut!(TX_USED.0),
        }
    };
    if !tx_vq.setup(base, VQ_TX, &caps) {
        Uart.errorln("[CONSOLE] tx queue setup failed");
        return;
    }

    if !set_driver_ok(base) {
        return;
    }
    *CONSOLE.lock() = Some(ConsoleDevice { tx_vq, ready: true });
    kprintln!("[CONSOLE] DRIVER_OK; tx-only path live");

    let banner = b"[Fermi OS] virtio-console attached. Hello from guest!\n";
    send(banner);
}
