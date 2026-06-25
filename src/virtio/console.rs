//! VirtIO console (port 0, TX-only) — a host-side log side-channel.
//!
//! Port of `src/pci/virtio/console/console.c`.

use super::virtqueue::{self, Virtqueue, VirtqAvail, VirtqDesc, VirtqUsed, VirtqUsedElem, VIRTQ_MAX_SIZE};
use super::*;
use crate::mm::mmu::virt_to_phys;
use crate::pci;
use crate::sync::Racy;
use crate::uart;

pub const VIRTIO_CONSOLE_VENDOR_ID: u16 = 0x1AF4;
pub const VIRTIO_CONSOLE_DEVICE_ID: u16 = 0x1043;
const VQ_RX: u16 = 0;
const VQ_TX: u16 = 1;
const CONSOLE_TX_BUF: usize = 4096;

#[repr(C, align(4096))]
struct ConBacking {
    rx_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    rx_avail: VirtqAvail,
    rx_used: VirtqUsed,
    tx_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    tx_avail: VirtqAvail,
    tx_used: VirtqUsed,
    tx_buf: [u8; CONSOLE_TX_BUF],
}

const ED: VirtqDesc = VirtqDesc { addr: 0, len: 0, flags: 0, next: 0 };
const EA: VirtqAvail = VirtqAvail { flags: 0, idx: 0, ring: [0; VIRTQ_MAX_SIZE] };
const EU: VirtqUsed = VirtqUsed { flags: 0, idx: 0, ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE] };

static BACKING: Racy<ConBacking> = Racy::new(ConBacking {
    rx_desc: [ED; VIRTQ_MAX_SIZE], rx_avail: EA, rx_used: EU,
    tx_desc: [ED; VIRTQ_MAX_SIZE], tx_avail: EA, tx_used: EU,
    tx_buf: [0; CONSOLE_TX_BUF],
});

struct ConDev {
    rx_vq: Virtqueue,
    tx_vq: Virtqueue,
    ready: bool,
}
static CON: Racy<ConDev> = Racy::new(ConDev {
    rx_vq: Virtqueue::empty(),
    tx_vq: Virtqueue::empty(),
    ready: false,
});

/// Push bytes onto the TX virtqueue (host sees them on its chardev backend).
pub fn send(buf: &[u8]) -> i32 {
    let con = unsafe { CON.get() };
    if !con.ready {
        return -1;
    }
    let b = unsafe { BACKING.get() };
    let mut done = 0usize;
    while done < buf.len() {
        let chunk = core::cmp::min(buf.len() - done, CONSOLE_TX_BUF);
        b.tx_buf[..chunk].copy_from_slice(&buf[done..done + chunk]);
        let pa = virt_to_phys(b.tx_buf.as_ptr() as u64);
        virtqueue::submit(&mut con.tx_vq, pa, chunk as u32, 0);
        virtqueue::notify(&con.tx_vq);
        virtqueue::poll(&mut con.tx_vq);
        done += chunk;
    }
    done as i32
}

pub fn is_ready() -> bool {
    unsafe { CON.get() }.ready
}

pub fn init() {
    uart::println("[CONSOLE] Initializing Device");
    let mut pdev = match pci::find_device(VIRTIO_CONSOLE_VENDOR_ID, VIRTIO_CONSOLE_DEVICE_ID) {
        Some(d) => d,
        None => {
            uart::println("[CONSOLE] Device not found (skipping)");
            return;
        }
    };
    pci::assign_bars(&mut pdev);
    pci::enable_device(&pdev);
    let con = unsafe { CON.get() };
    let mut caps = VirtioPciCaps::default();
    super::parse_capabilities(&pdev, &mut caps);
    let base = caps.common_cfg;

    if super::handshake(base, 0).is_none() {
        uart::errorln("[CONSOLE] handshake failed");
        return;
    }
    let b = unsafe { BACKING.get() };
    con.rx_vq.desc = b.rx_desc.as_mut_ptr();
    con.rx_vq.avail = &mut b.rx_avail as *mut VirtqAvail;
    con.rx_vq.used = &mut b.rx_used as *mut VirtqUsed;
    if !virtqueue::setup(base, VQ_RX, &mut con.rx_vq, &caps) {
        uart::errorln("[CONSOLE] rx queue setup failed");
        return;
    }
    con.tx_vq.desc = b.tx_desc.as_mut_ptr();
    con.tx_vq.avail = &mut b.tx_avail as *mut VirtqAvail;
    con.tx_vq.used = &mut b.tx_used as *mut VirtqUsed;
    if !virtqueue::setup(base, VQ_TX, &mut con.tx_vq, &caps) {
        uart::errorln("[CONSOLE] tx queue setup failed");
        return;
    }
    super::set_driver_ok(base);
    con.ready = true;
    uart::println("[CONSOLE] DRIVER_OK; tx-only path live");
    let banner = b"[Fermi OS] virtio-console attached. Hello from guest!\n";
    send(banner);
}
