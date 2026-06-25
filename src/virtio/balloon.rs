//! VirtIO memory balloon — cooperative inflate/deflate.
//!
//! Port of `src/pci/virtio/balloon/balloon.c`. Driver-initiated (no config
//! interrupt): the shell drives inflate/deflate and /proc/balloon shows drift.

use super::virtqueue::{self, Virtqueue, VirtqAvail, VirtqDesc, VirtqUsed, VirtqUsedElem, VIRTQ_MAX_SIZE};
use super::*;
use crate::mm::mmu::virt_to_phys;
use crate::mm::pmm;
use crate::mmio;
use crate::pci;
use crate::sync::Racy;
use crate::uart;

pub const VIRTIO_BALLOON_VENDOR_ID: u16 = 0x1AF4;
pub const VIRTIO_BALLOON_DEVICE_ID: u16 = 0x1045;
const PFN_SHIFT: u64 = 12;
const MAX_PAGES: usize = 1024;
const VQ_INFLATE: u16 = 0;
const VQ_DEFLATE: u16 = 1;
const CFG_NUM_PAGES: usize = 0x00;
const CFG_ACTUAL: usize = 0x04;

#[repr(C, align(4096))]
struct BalBacking {
    inf_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    inf_avail: VirtqAvail,
    inf_used: VirtqUsed,
    def_desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    def_avail: VirtqAvail,
    def_used: VirtqUsed,
    pfn_buf: [u32; MAX_PAGES],
}
const ED: VirtqDesc = VirtqDesc { addr: 0, len: 0, flags: 0, next: 0 };
const EA: VirtqAvail = VirtqAvail { flags: 0, idx: 0, ring: [0; VIRTQ_MAX_SIZE] };
const EU: VirtqUsed = VirtqUsed { flags: 0, idx: 0, ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE] };

static BACKING: Racy<BalBacking> = Racy::new(BalBacking {
    inf_desc: [ED; VIRTQ_MAX_SIZE], inf_avail: EA, inf_used: EU,
    def_desc: [ED; VIRTQ_MAX_SIZE], def_avail: EA, def_used: EU,
    pfn_buf: [0; MAX_PAGES],
});

struct BalDev {
    caps: VirtioPciCaps,
    inflate_vq: Virtqueue,
    deflate_vq: Virtqueue,
    actual: u32,
    inflated_pfns: [u32; MAX_PAGES],
    ready: bool,
}
static BAL: Racy<BalDev> = Racy::new(BalDev {
    caps: VirtioPciCaps { common_cfg: 0, notify_base: 0, isr_cfg: 0, device_cfg: 0, notify_off_multiplier: 0 },
    inflate_vq: Virtqueue::empty(),
    deflate_vq: Virtqueue::empty(),
    actual: 0,
    inflated_pfns: [0; MAX_PAGES],
    ready: false,
});

fn submit_pfn_batch(vq: &mut Virtqueue, count: u32) {
    if count == 0 {
        return;
    }
    let b = unsafe { BACKING.get() };
    let pa = virt_to_phys(b.pfn_buf.as_ptr() as u64);
    virtqueue::submit(vq, pa, count * 4, 0);
    virtqueue::notify(vq);
    virtqueue::poll(vq);
}

fn publish_actual() {
    let bal = unsafe { BAL.get() };
    if bal.caps.device_cfg != 0 {
        mmio::write32(bal.caps.device_cfg + CFG_ACTUAL, bal.actual);
        dsb_sy();
    }
}

pub fn inflate(mut n: u32) -> i32 {
    let bal = unsafe { BAL.get() };
    if !bal.ready {
        return -1;
    }
    let headroom = MAX_PAGES as u32 - bal.actual;
    if n > headroom {
        n = headroom;
    }
    if n == 0 {
        return 0;
    }
    let b = unsafe { BACKING.get() };
    let mut got = 0u32;
    for _ in 0..n {
        let pa = pmm::allocate_page();
        if pa == 0 {
            break;
        }
        let pfn = (pa >> PFN_SHIFT) as u32;
        b.pfn_buf[got as usize] = pfn;
        bal.inflated_pfns[(bal.actual + got) as usize] = pfn;
        got += 1;
    }
    if got == 0 {
        return 0;
    }
    submit_pfn_batch(&mut bal.inflate_vq, got);
    bal.actual += got;
    publish_actual();
    got as i32
}

pub fn deflate(mut n: u32) -> i32 {
    let bal = unsafe { BAL.get() };
    if !bal.ready {
        return -1;
    }
    if n > bal.actual {
        n = bal.actual;
    }
    if n == 0 {
        return 0;
    }
    let b = unsafe { BACKING.get() };
    let base = bal.actual - n;
    for i in 0..n {
        b.pfn_buf[i as usize] = bal.inflated_pfns[(base + i) as usize];
    }
    submit_pfn_batch(&mut bal.deflate_vq, n);
    for i in 0..n {
        pmm::free_page((b.pfn_buf[i as usize] as u64) << PFN_SHIFT);
    }
    bal.actual -= n;
    publish_actual();
    n as i32
}

/// (actual_pages, host_target).
pub fn status() -> (u32, u32) {
    let bal = unsafe { BAL.get() };
    if !bal.ready {
        return (0, 0);
    }
    let target = if bal.caps.device_cfg != 0 {
        mmio::read32(bal.caps.device_cfg + CFG_NUM_PAGES)
    } else {
        0
    };
    (bal.actual, target)
}

pub fn init() {
    uart::println("[BALLOON] Initializing Device");
    let mut pdev = match pci::find_device(VIRTIO_BALLOON_VENDOR_ID, VIRTIO_BALLOON_DEVICE_ID) {
        Some(d) => d,
        None => {
            uart::println("[BALLOON] Device not found (skipping)");
            return;
        }
    };
    pci::assign_bars(&mut pdev);
    pci::enable_device(&pdev);
    let bal = unsafe { BAL.get() };
    super::parse_capabilities(&pdev, &mut bal.caps);
    let base = bal.caps.common_cfg;
    if super::handshake(base, 0).is_none() {
        uart::errorln("[BALLOON] handshake failed");
        return;
    }
    let b = unsafe { BACKING.get() };
    bal.inflate_vq.desc = b.inf_desc.as_mut_ptr();
    bal.inflate_vq.avail = &mut b.inf_avail as *mut VirtqAvail;
    bal.inflate_vq.used = &mut b.inf_used as *mut VirtqUsed;
    if !virtqueue::setup(base, VQ_INFLATE, &mut bal.inflate_vq, &bal.caps) {
        uart::errorln("[BALLOON] inflate queue setup failed");
        return;
    }
    bal.deflate_vq.desc = b.def_desc.as_mut_ptr();
    bal.deflate_vq.avail = &mut b.def_avail as *mut VirtqAvail;
    bal.deflate_vq.used = &mut b.def_used as *mut VirtqUsed;
    if !virtqueue::setup(base, VQ_DEFLATE, &mut bal.deflate_vq, &bal.caps) {
        uart::errorln("[BALLOON] deflate queue setup failed");
        return;
    }
    super::set_driver_ok(base);
    bal.ready = true;
    uart::println("[BALLOON] DRIVER_OK");
}
