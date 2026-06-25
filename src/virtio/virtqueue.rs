//! Split virtqueue — descriptor table, available ring, used ring.
//!
//! Port of `src/pci/virtio/virtqueue.c`.

use super::*;
use crate::kprintln;
use crate::mm::mmu::virt_to_phys;
use crate::mmio;
use crate::uart;

pub const VIRTQ_DESC_F_NEXT: u16 = 1;
pub const VIRTQ_DESC_F_WRITE: u16 = 2;
pub const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;
pub const VIRTQ_MAX_SIZE: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqDesc {
    pub addr: u64,
    pub len: u32,
    pub flags: u16,
    pub next: u16,
}

#[repr(C)]
pub struct VirtqAvail {
    pub flags: u16,
    pub idx: u16,
    pub ring: [u16; VIRTQ_MAX_SIZE],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqUsedElem {
    pub id: u32,
    pub len: u32,
}

#[repr(C)]
pub struct VirtqUsed {
    pub flags: u16,
    pub idx: u16,
    pub ring: [VirtqUsedElem; VIRTQ_MAX_SIZE],
}

/// A single segment for chained submission.
#[derive(Clone, Copy)]
pub struct VirtqSeg {
    pub pa: u64,
    pub len: u32,
    pub flags: u16,
}

/// Virtqueue state. `desc`/`avail`/`used` point at page-aligned backing memory
/// owned by the driver.
pub struct Virtqueue {
    pub size: u16,
    pub free_head: u16,
    pub last_used: u16,
    pub notify_addr: usize,
    pub desc: *mut VirtqDesc,
    pub avail: *mut VirtqAvail,
    pub used: *mut VirtqUsed,
}

impl Virtqueue {
    pub const fn empty() -> Self {
        Self {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::null_mut(),
            avail: core::ptr::null_mut(),
            used: core::ptr::null_mut(),
        }
    }
}

/// Configure a virtqueue via the common-config MMIO registers. Returns false
/// if the queue is unavailable.
pub fn setup(base: usize, queue_idx: u16, vq: &mut Virtqueue, caps: &VirtioPciCaps) -> bool {
    mmio::write16(base + VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);
    dsb_sy();
    mmio::write16(base + VIRTIO_COMMON_Q_SELECT, queue_idx);
    dsb_sy();

    let max_size = mmio::read16(base + VIRTIO_COMMON_Q_SIZE);
    kprintln!("[VQ] Queue {} max size: {}", queue_idx, max_size);
    if max_size == 0 {
        uart::errorln("[VQ] Queue not available");
        return false;
    }
    let qsize = core::cmp::min(VIRTQ_MAX_SIZE as u16, max_size);
    mmio::write16(base + VIRTIO_COMMON_Q_SIZE, qsize);
    mmio::write16(base + VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
    dsb_sy();

    unsafe {
        core::ptr::write_bytes(vq.desc as *mut u8, 0, core::mem::size_of::<VirtqDesc>() * qsize as usize);
        core::ptr::write_bytes(vq.avail as *mut u8, 0, core::mem::size_of::<VirtqAvail>());
        core::ptr::write_bytes(vq.used as *mut u8, 0, core::mem::size_of::<VirtqUsed>());
    }

    let desc_pa = virt_to_phys(vq.desc as u64);
    mmio::write32(base + VIRTIO_COMMON_Q_DESCLO, desc_pa as u32);
    mmio::write32(base + VIRTIO_COMMON_Q_DESCHI, (desc_pa >> 32) as u32);
    let avail_pa = virt_to_phys(vq.avail as u64);
    mmio::write32(base + VIRTIO_COMMON_Q_DRIVERLO, avail_pa as u32);
    mmio::write32(base + VIRTIO_COMMON_Q_DRIVERHI, (avail_pa >> 32) as u32);
    let used_pa = virt_to_phys(vq.used as u64);
    mmio::write32(base + VIRTIO_COMMON_Q_DEVICELO, used_pa as u32);
    mmio::write32(base + VIRTIO_COMMON_Q_DEVICEHI, (used_pa >> 32) as u32);
    dsb_sy();

    let notify_off = mmio::read16(base + VIRTIO_COMMON_Q_NOFF);
    vq.notify_addr = caps.notify_base + (notify_off as usize) * (caps.notify_off_multiplier as usize);
    vq.size = qsize;
    vq.free_head = 0;
    vq.last_used = 0;

    mmio::write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
    dsb_sy();
    kprintln!("[VQ] Queue {} enabled (size={})", queue_idx, qsize);
    true
}

/// Add a single buffer to the available ring.
pub fn submit(vq: &mut Virtqueue, buf_pa: u64, len: u32, flags: u16) {
    let idx = vq.free_head;
    vq.free_head = (idx + 1) % vq.size;
    unsafe {
        let d = vq.desc.add(idx as usize);
        (*d).addr = buf_pa;
        (*d).len = len;
        (*d).flags = flags;
        (*d).next = 0;
        let avail = &mut *vq.avail;
        let ai = avail.idx;
        avail.ring[(ai % vq.size) as usize] = idx;
        dsb_sy();
        avail.idx = ai + 1;
        dsb_sy();
    }
}

/// Add a chain of N linked descriptors; returns the head index.
pub fn submit_chain(vq: &mut Virtqueue, segs: &[VirtqSeg]) -> u16 {
    let n = segs.len() as u16;
    let head = vq.free_head;
    unsafe {
        for i in 0..n {
            let idx = (head + i) % vq.size;
            let mut flags = segs[i as usize].flags;
            if i < n - 1 {
                flags |= VIRTQ_DESC_F_NEXT;
            }
            let d = vq.desc.add(idx as usize);
            (*d).addr = segs[i as usize].pa;
            (*d).len = segs[i as usize].len;
            (*d).flags = flags;
            (*d).next = if i < n - 1 { (head + i + 1) % vq.size } else { 0 };
        }
        vq.free_head = (head + n) % vq.size;
        let avail = &mut *vq.avail;
        let ai = avail.idx;
        avail.ring[(ai % vq.size) as usize] = head;
        dsb_sy();
        avail.idx = ai + 1;
        dsb_sy();
    }
    head
}

pub fn notify(vq: &Virtqueue) {
    mmio::write16(vq.notify_addr, 0);
}

const POLL_MAX_SPINS: u32 = 10_000_000;

/// Spin until the device produces a used entry; returns bytes written (0 on timeout).
pub fn poll(vq: &mut Virtqueue) -> u32 {
    let mut spins = 0u32;
    unsafe {
        while core::ptr::read_volatile(&(*vq.used).idx) == vq.last_used {
            spins += 1;
            if spins >= POLL_MAX_SPINS {
                uart::errorln("[VQ] poll: timeout waiting for device");
                return 0;
            }
        }
        dsb_sy();
        let used_idx = (vq.last_used % vq.size) as usize;
        let written = (*vq.used).ring[used_idx].len;
        vq.last_used += 1;
        written
    }
}
