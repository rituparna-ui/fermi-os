//! VirtIO network driver core — dual RX/TX virtqueues, MAC/link readout,
//! RX buffer pre-fill, synchronous `tx`/`rx_poll`.
//!
//! Port of the driver portion of `src/pci/virtio/net/net.c`. The L3 protocol
//! helpers (ARP/IPv4/ICMP/DHCP) are added at the networking milestone.

use super::virtqueue::{
    self, Virtqueue, VirtqAvail, VirtqDesc, VirtqSeg, VirtqUsed, VirtqUsedElem, VIRTQ_DESC_F_WRITE,
    VIRTQ_MAX_SIZE,
};
use super::*;
use crate::kprintln;
use crate::mm::mmu::virt_to_phys;
use crate::mmio;
use crate::pci;
use crate::sync::Racy;
use crate::uart;

pub const VIRTIO_NET_VENDOR_ID: u16 = 0x1AF4;
pub const VIRTIO_NET_DEVICE_ID: u16 = 0x1041;

const VIRTIO_NET_CFG_MAC: usize = 0x00;
const VIRTIO_NET_CFG_STATUS: usize = 0x06;
pub const VIRTIO_NET_S_LINK_UP: u16 = 1 << 0;
const VIRTIO_NET_F_MAC: u32 = 1 << 5;
const VIRTIO_NET_F_STATUS: u32 = 1 << 16;

pub const VIRTIO_NET_HDR_LEN: u32 = 12;
const QUEUE_RX: u16 = 0;
const QUEUE_TX: u16 = 1;

const NET_RX_BUF_COUNT: usize = 8;
const NET_RX_BUF_SIZE: usize = 1600;

#[repr(C, align(4096))]
struct NetBacking {
    rx_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    rx_avail: VirtqAvail,
    rx_used: VirtqUsed,
    tx_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    tx_avail: VirtqAvail,
    tx_used: VirtqUsed,
    rx_bufs: [[u8; NET_RX_BUF_SIZE]; NET_RX_BUF_COUNT],
    tx_hdr: [u8; 16],
}

const EMPTY_DESC: VirtqDesc = VirtqDesc { addr: 0, len: 0, flags: 0, next: 0 };
const EMPTY_AVAIL: VirtqAvail = VirtqAvail { flags: 0, idx: 0, ring: [0; VIRTQ_MAX_SIZE] };
const EMPTY_USED: VirtqUsed = VirtqUsed {
    flags: 0,
    idx: 0,
    ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
};

static BACKING: Racy<NetBacking> = Racy::new(NetBacking {
    rx_desc: [EMPTY_DESC; VIRTQ_MAX_SIZE],
    rx_avail: EMPTY_AVAIL,
    rx_used: EMPTY_USED,
    tx_desc: [EMPTY_DESC; VIRTQ_MAX_SIZE],
    tx_avail: EMPTY_AVAIL,
    tx_used: EMPTY_USED,
    rx_bufs: [[0; NET_RX_BUF_SIZE]; NET_RX_BUF_COUNT],
    tx_hdr: [0; 16],
});

pub struct NetDev {
    caps: VirtioPciCaps,
    rx_vq: Virtqueue,
    tx_vq: Virtqueue,
    pub mac: [u8; 6],
    pub have_mac: bool,
    pub link_status: u16,
    rx_desc_to_buf: [i32; VIRTQ_MAX_SIZE],
    rx_initialized: bool,
    pub rx_packets: u64,
    pub tx_packets: u64,
    pub ready: bool,
    pub irq_intid: u32,
    pub rx_irqs: u64,
}

static NET: Racy<NetDev> = Racy::new(NetDev {
    caps: VirtioPciCaps {
        common_cfg: 0,
        notify_base: 0,
        isr_cfg: 0,
        device_cfg: 0,
        notify_off_multiplier: 0,
    },
    rx_vq: Virtqueue::empty(),
    tx_vq: Virtqueue::empty(),
    mac: [0; 6],
    have_mac: false,
    link_status: 0,
    rx_desc_to_buf: [-1; VIRTQ_MAX_SIZE],
    rx_initialized: false,
    rx_packets: 0,
    tx_packets: 0,
    ready: false,
    irq_intid: 0,
    rx_irqs: 0,
});

pub(crate) fn dev() -> &'static mut NetDev {
    unsafe { NET.get() }
}
fn backing() -> &'static mut NetBacking {
    unsafe { BACKING.get() }
}

fn rx_submit_buf(buf_idx: usize) {
    let nd = dev();
    let b = backing();
    let desc_id = nd.rx_vq.free_head;
    let pa = virt_to_phys(b.rx_bufs[buf_idx].as_ptr() as u64);
    virtqueue::submit(&mut nd.rx_vq, pa, NET_RX_BUF_SIZE as u32, VIRTQ_DESC_F_WRITE);
    nd.rx_desc_to_buf[desc_id as usize] = buf_idx as i32;
}

fn rx_init() {
    let nd = dev();
    for i in 0..VIRTQ_MAX_SIZE {
        nd.rx_desc_to_buf[i] = -1;
    }
    for i in 0..NET_RX_BUF_COUNT {
        rx_submit_buf(i);
    }
    virtqueue::notify(&dev().rx_vq);
    dev().rx_initialized = true;
    kprintln!(
        "[NET] RX queue primed with {} buffers ({} bytes each)",
        NET_RX_BUF_COUNT,
        NET_RX_BUF_SIZE
    );
}

/// Send a raw Ethernet frame (the driver prepends the virtio_net_hdr).
pub fn tx(frame: &[u8]) -> i32 {
    let nd = dev();
    if !nd.ready || frame.is_empty() {
        return -1;
    }
    let b = backing();
    for x in b.tx_hdr.iter_mut() {
        *x = 0;
    }
    let hdr_pa = virt_to_phys(b.tx_hdr.as_ptr() as u64);
    let frame_pa = virt_to_phys(frame.as_ptr() as u64);
    let segs = [
        VirtqSeg { pa: hdr_pa, len: VIRTIO_NET_HDR_LEN, flags: 0 },
        VirtqSeg { pa: frame_pa, len: frame.len() as u32, flags: 0 },
    ];
    virtqueue::submit_chain(&mut nd.tx_vq, &segs);
    virtqueue::notify(&nd.tx_vq);
    virtqueue::poll(&mut nd.tx_vq);
    nd.tx_packets += 1;
    frame.len() as i32
}

/// Drain one received frame (sans virtio_net_hdr) into `dst`. Returns bytes
/// copied, 0 if nothing pending, -1 on error.
pub fn rx_poll(dst: &mut [u8]) -> i32 {
    let nd = dev();
    if !nd.rx_initialized {
        return -1;
    }
    let used_now = unsafe { core::ptr::read_volatile(&(*nd.rx_vq.used).idx) };
    if nd.rx_vq.last_used == used_now {
        return 0;
    }
    dsb_sy();
    let slot = (nd.rx_vq.last_used % nd.rx_vq.size) as usize;
    let (desc_id, total_len) = unsafe {
        let e = (*nd.rx_vq.used).ring[slot];
        (e.id, e.len)
    };
    nd.rx_vq.last_used += 1;

    if desc_id as usize >= VIRTQ_MAX_SIZE || nd.rx_desc_to_buf[desc_id as usize] < 0 {
        uart::errorln("[NET] rx_poll: bogus/unmapped descriptor");
        return -1;
    }
    let buf_idx = nd.rx_desc_to_buf[desc_id as usize] as usize;
    nd.rx_desc_to_buf[desc_id as usize] = -1;

    let b = backing();
    let mut copied = 0i32;
    if total_len >= VIRTIO_NET_HDR_LEN {
        let frame_len = (total_len - VIRTIO_NET_HDR_LEN) as usize;
        let to_copy = core::cmp::min(frame_len, dst.len());
        if to_copy > 0 {
            let src = &b.rx_bufs[buf_idx][VIRTIO_NET_HDR_LEN as usize..VIRTIO_NET_HDR_LEN as usize + to_copy];
            dst[..to_copy].copy_from_slice(src);
        }
        copied = to_copy as i32;
    }
    rx_submit_buf(buf_idx);
    virtqueue::notify(&dev().rx_vq);
    if copied > 0 {
        dev().rx_packets += 1;
    }
    copied
}

pub fn init() {
    uart::println("[NET] Initializing Device");
    let mut pdev = match pci::find_device(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID) {
        Some(d) => d,
        None => {
            uart::errorln("[NET] Device not found");
            return;
        }
    };
    if pci::header_type(&pdev) & 0x7F != pci::PCI_ENDPOINT_DEV_TYPE {
        uart::errorln("[NET] Unexpected header type");
        return;
    }
    pci::assign_bars(&mut pdev);
    pci::enable_device(&pdev);

    let nd = dev();
    super::parse_capabilities(&pdev, &mut nd.caps);
    let base = nd.caps.common_cfg;

    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}
    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 0);
    dsb_sy();
    let feat_lo = mmio::read32(base + VIRTIO_COMMON_DF);
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 1);
    dsb_sy();
    let feat_hi = mmio::read32(base + VIRTIO_COMMON_DF);
    if feat_hi & 0x01 == 0 {
        uart::errorln("[NET] Device lacks VIRTIO_F_VERSION_1");
        return;
    }
    let want_lo = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    let guest_lo = feat_lo & want_lo;
    let guest_hi = feat_hi & 0x01;
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_lo);
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
        uart::errorln("[NET] FEATURES_OK failed");
        return;
    }

    let b = backing();
    nd.rx_vq.desc = b.rx_desc.as_mut_ptr();
    nd.rx_vq.avail = &mut b.rx_avail as *mut VirtqAvail;
    nd.rx_vq.used = &mut b.rx_used as *mut VirtqUsed;
    if !virtqueue::setup(base, QUEUE_RX, &mut nd.rx_vq, &nd.caps) {
        uart::errorln("[NET] RX virtqueue setup failed");
        return;
    }
    nd.tx_vq.desc = b.tx_desc.as_mut_ptr();
    nd.tx_vq.avail = &mut b.tx_avail as *mut VirtqAvail;
    nd.tx_vq.used = &mut b.tx_used as *mut VirtqUsed;
    if !virtqueue::setup(base, QUEUE_TX, &mut nd.tx_vq, &nd.caps) {
        uart::errorln("[NET] TX virtqueue setup failed");
        return;
    }

    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    dsb_sy();
    nd.ready = true;

    rx_init();

    // Route the device's legacy INTx to the GIC so RX is interrupt-driven.
    nd.irq_intid = pci::device_intid(&pdev);
    if nd.irq_intid != 0 {
        crate::exception::gic::enable_irq(nd.irq_intid);
        kprintln!("[NET] INTx -> GIC INTID {}", nd.irq_intid);
    }

    let dcfg = nd.caps.device_cfg;
    if guest_lo & VIRTIO_NET_F_MAC != 0 {
        for i in 0..6 {
            nd.mac[i] = mmio::read8(dcfg + VIRTIO_NET_CFG_MAC + i);
        }
        nd.have_mac = true;
        kprintln!(
            "[NET] MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            nd.mac[0], nd.mac[1], nd.mac[2], nd.mac[3], nd.mac[4], nd.mac[5]
        );
    }
    if guest_lo & VIRTIO_NET_F_STATUS != 0 {
        nd.link_status = mmio::read16(dcfg + VIRTIO_NET_CFG_STATUS);
        kprintln!(
            "[NET] Link: {} (status={:#x})",
            if nd.link_status & VIRTIO_NET_S_LINK_UP != 0 { "UP" } else { "DOWN" },
            nd.link_status
        );
    }
}

/// GIC INTID assigned to the net device (0 if none).
pub fn irq_intid() -> u32 {
    dev().irq_intid
}

/// Net IRQ handler: read+clear the VirtIO ISR status and count the event.
/// The RX ring is drained by consumers (rx_poll); this just acknowledges the
/// device-level interrupt so it deasserts.
pub fn handle_irq() {
    let nd = dev();
    if nd.caps.isr_cfg != 0 {
        let _isr = crate::mmio::read8(nd.caps.isr_cfg); // read clears it
    }
    nd.rx_irqs += 1;
}

pub fn rx_irq_count() -> u64 {
    dev().rx_irqs
}
