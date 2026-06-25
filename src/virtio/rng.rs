//! VirtIO RNG driver — full device init + random byte generation.
//!
//! Port of `src/pci/virtio/rng/rng.c`.

use super::virtqueue::{self, Virtqueue, VirtqAvail, VirtqDesc, VirtqUsed, VIRTQ_DESC_F_WRITE, VIRTQ_MAX_SIZE};
use super::*;
use crate::mm::mmu::virt_to_phys;
use crate::mmio;
use crate::pci;
use crate::sync::Racy;
use crate::uart;

pub const VIRTIO_RNG_VENDOR_ID: u16 = 0x1AF4;
pub const VIRTIO_RNG_DEVICE_ID: u16 = 0x1044;

const RNG_BUF_SIZE: usize = 256;

/// Page-aligned DMA backing memory for the virtqueue + bounce buffer.
#[repr(C, align(4096))]
struct RngBacking {
    desc: [VirtqDesc; VIRTQ_MAX_SIZE],
    avail: VirtqAvail,
    used: VirtqUsed,
    buf: [u8; RNG_BUF_SIZE],
}

static BACKING: Racy<RngBacking> = Racy::new(RngBacking {
    desc: [VirtqDesc {
        addr: 0,
        len: 0,
        flags: 0,
        next: 0,
    }; VIRTQ_MAX_SIZE],
    avail: VirtqAvail {
        flags: 0,
        idx: 0,
        ring: [0; VIRTQ_MAX_SIZE],
    },
    used: VirtqUsed {
        flags: 0,
        idx: 0,
        ring: [virtqueue::VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
    },
    buf: [0; RNG_BUF_SIZE],
});

struct RngDev {
    caps: VirtioPciCaps,
    vq: Virtqueue,
    ready: bool,
}

static RNG: Racy<RngDev> = Racy::new(RngDev {
    caps: VirtioPciCaps {
        common_cfg: 0,
        notify_base: 0,
        isr_cfg: 0,
        device_cfg: 0,
        notify_off_multiplier: 0,
    },
    vq: Virtqueue::empty(),
    ready: false,
});

pub fn init() {
    uart::println("[RNG] Initializing Device");
    let mut dev = match pci::find_device(VIRTIO_RNG_VENDOR_ID, VIRTIO_RNG_DEVICE_ID) {
        Some(d) => d,
        None => {
            uart::errorln("[RNG] Device not found");
            return;
        }
    };
    if pci::header_type(&dev) & 0x7F != pci::PCI_ENDPOINT_DEV_TYPE {
        uart::errorln("[RNG] Unexpected header type");
        return;
    }
    pci::assign_bars(&mut dev);
    pci::enable_device(&dev);

    let rng = unsafe { RNG.get() };
    pci::config_read16(dev.bus as u16, dev.slot, dev.func, pci::PCI_STATUS); // touch
    super::parse_capabilities(&dev, &mut rng.caps);

    let base = rng.caps.common_cfg;

    // VirtIO init sequence.
    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}

    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    // Feature negotiation: accept only VIRTIO_F_VERSION_1 (bit 32).
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
        uart::errorln("[RNG] FEATURES_OK failed");
        return;
    }

    // Point the virtqueue at the static backing memory.
    let backing = unsafe { BACKING.get() };
    rng.vq.desc = backing.desc.as_mut_ptr();
    rng.vq.avail = &mut backing.avail as *mut VirtqAvail;
    rng.vq.used = &mut backing.used as *mut VirtqUsed;

    if !virtqueue::setup(base, 0, &mut rng.vq, &rng.caps) {
        uart::errorln("[RNG] Virtqueue setup failed");
        return;
    }

    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    dsb_sy();
    rng.ready = true;
    uart::println("[RNG] DRIVER_OK set");
}

/// Fill `dst` with random bytes from the device. Returns bytes produced.
pub fn read(dst: &mut [u8]) -> usize {
    let rng = unsafe { RNG.get() };
    if !rng.ready {
        return 0;
    }
    let backing = unsafe { BACKING.get() };
    let buf_pa = virt_to_phys(backing.buf.as_ptr() as u64);
    let mut done = 0usize;
    while done < dst.len() {
        let chunk = core::cmp::min(dst.len() - done, RNG_BUF_SIZE);
        virtqueue::submit(&mut rng.vq, buf_pa, chunk as u32, VIRTQ_DESC_F_WRITE);
        virtqueue::notify(&rng.vq);
        virtqueue::poll(&mut rng.vq);
        dst[done..done + chunk].copy_from_slice(&backing.buf[..chunk]);
        done += chunk;
    }
    done
}
