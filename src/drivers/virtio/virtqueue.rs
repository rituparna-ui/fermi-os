//! Split virtqueue: descriptor table, available ring, used ring, plus submit/
//! notify/poll. Backing memory is provided by each driver as kernel-VA statics;
//! ring physical addresses (for the device) are derived via `virt_to_phys`.

use crate::arch::cpu::dsb_sy;
use crate::drivers::virtio::*;
use crate::klib::mmio;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;

// Descriptor flags.
pub const VIRTQ_DESC_F_NONE: u16 = 0;
pub const VIRTQ_DESC_F_NEXT: u16 = 1;
pub const VIRTQ_DESC_F_WRITE: u16 = 2;

/// MSI-X "no vector" (polling mode).
pub const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

/// Max descriptors per queue.
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

/// A complete split virtqueue. `desc`/`avail`/`used` point at driver-owned,
/// page-aligned kernel-VA storage.
pub struct Virtqueue {
    pub size: u16,
    pub free_head: u16,
    pub last_used: u16,
    pub notify_addr: usize,
    pub desc: *mut VirtqDesc,
    pub avail: *mut VirtqAvail,
    pub used: *mut VirtqUsed,
}

/// A single descriptor segment for chain submission.
#[derive(Clone, Copy)]
pub struct VirtqSeg {
    pub pa: u64,
    pub len: u32,
    pub flags: u16,
}

const POLL_MAX_SPINS: u32 = 10_000_000;

impl Virtqueue {
    /// Configure the queue via the common-config MMIO registers. `base` is the
    /// physical common-cfg address. Returns false on failure.
    pub fn setup(&mut self, base: usize, queue_idx: u16, caps: &VirtioPciCaps) -> bool {
        // Polling mode: no MSI-X for config changes.
        mmio::write16(base + VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);
        dsb_sy();

        mmio::write16(base + VIRTIO_COMMON_Q_SELECT, queue_idx);
        dsb_sy();

        let max_size = mmio::read16(base + VIRTIO_COMMON_Q_SIZE);
        kprintln!("[VQ] Queue {} max size: {}", queue_idx, max_size);
        if max_size == 0 {
            Uart.errorln("[VQ] Queue not available");
            return false;
        }

        let qsize = core::cmp::min(VIRTQ_MAX_SIZE as u16, max_size);
        mmio::write16(base + VIRTIO_COMMON_Q_SIZE, qsize);

        mmio::write16(base + VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
        dsb_sy();

        // Zero the rings.
        unsafe {
            core::ptr::write_bytes(self.desc, 0, qsize as usize);
            core::ptr::write_bytes(self.avail, 0, 1);
            core::ptr::write_bytes(self.used, 0, 1);
        }

        // Hand ring physical addresses to the device (low/high halves).
        let desc_pa = virt_to_phys(self.desc as u64);
        mmio::write32(base + VIRTIO_COMMON_Q_DESCLO, desc_pa as u32);
        mmio::write32(base + VIRTIO_COMMON_Q_DESCHI, (desc_pa >> 32) as u32);

        let avail_pa = virt_to_phys(self.avail as u64);
        mmio::write32(base + VIRTIO_COMMON_Q_DRIVERLO, avail_pa as u32);
        mmio::write32(base + VIRTIO_COMMON_Q_DRIVERHI, (avail_pa >> 32) as u32);

        let used_pa = virt_to_phys(self.used as u64);
        mmio::write32(base + VIRTIO_COMMON_Q_DEVICELO, used_pa as u32);
        mmio::write32(base + VIRTIO_COMMON_Q_DEVICEHI, (used_pa >> 32) as u32);
        dsb_sy();

        let notify_off = mmio::read16(base + VIRTIO_COMMON_Q_NOFF);
        self.notify_addr =
            caps.notify_base + (notify_off as usize) * (caps.notify_off_multiplier as usize);
        kprintln!(
            "[VQ] Notify offset={} addr={:#x}",
            notify_off,
            self.notify_addr
        );

        self.size = qsize;
        self.free_head = 0;
        self.last_used = 0;

        mmio::write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
        dsb_sy();

        kprintln!("[VQ] Queue {} enabled (size={})", queue_idx, qsize);
        true
    }

    /// Add a single buffer to the available ring.
    pub fn submit(&mut self, buf_pa: u64, len: u32, flags: u16) {
        let idx = self.free_head;
        self.free_head = (idx + 1) % self.size;

        unsafe {
            let d = self.desc.add(idx as usize);
            (*d).addr = buf_pa;
            (*d).len = len;
            (*d).flags = flags;
            (*d).next = 0;
            // Publish the descriptor contents BEFORE the avail entry that points
            // at it — otherwise a weakly-ordered device could follow the ring
            // into a still-stale descriptor (VirtIO 1.0 §2.6).
            dsb_sy();

            let avail_idx = (*self.avail).idx;
            (*self.avail).ring[avail_idx as usize % self.size as usize] = idx;
            dsb_sy();
            (*self.avail).idx = avail_idx.wrapping_add(1);
            dsb_sy();
        }
    }

    /// Add a chain of `segs` linked descriptors; only the head is published.
    /// Returns the head descriptor index.
    pub fn submit_chain(&mut self, segs: &[VirtqSeg]) -> u16 {
        let n = segs.len() as u16;
        let head = self.free_head;

        unsafe {
            for (i, seg) in segs.iter().enumerate() {
                let i = i as u16;
                let idx = (head + i) % self.size;
                let mut flags = seg.flags;
                if i < n - 1 {
                    flags |= VIRTQ_DESC_F_NEXT;
                }
                let d = self.desc.add(idx as usize);
                (*d).addr = seg.pa;
                (*d).len = seg.len;
                (*d).flags = flags;
                (*d).next = if i < n - 1 { (head + i + 1) % self.size } else { 0 };
            }

            self.free_head = (head + n) % self.size;

            // Publish all N linked descriptors before the avail entry exposes
            // the chain head — the device may walk `next` into a descriptor
            // whose contents haven't landed yet otherwise.
            dsb_sy();
            let avail_idx = (*self.avail).idx;
            (*self.avail).ring[avail_idx as usize % self.size as usize] = head;
            dsb_sy();
            (*self.avail).idx = avail_idx.wrapping_add(1);
            dsb_sy();
        }
        head
    }

    /// Ring the notification doorbell (16-bit write per spec 4.1.4.4).
    pub fn notify(&self) {
        mmio::write16(self.notify_addr, 0);
    }

    /// Spin until the device produces a used entry; returns bytes written (0 on
    /// timeout).
    pub fn poll(&mut self) -> u32 {
        let mut spins = 0u32;
        unsafe {
            while core::ptr::read_volatile(&(*self.used).idx) == self.last_used {
                spins += 1;
                if spins >= POLL_MAX_SPINS {
                    Uart.errorln("[VQ] virtqueue_poll: timeout waiting for device");
                    return 0;
                }
            }
            dsb_sy();
            let used_idx = self.last_used as usize % self.size as usize;
            // Volatile: dsb_sy() orders the CPU but is not a compiler barrier
            // (it has no memory clobber), so read the device-written element
            // through a volatile load to stop the optimizer caching it.
            let written = core::ptr::read_volatile(&(*self.used).ring[used_idx].len);
            self.last_used = self.last_used.wrapping_add(1);
            written
        }
    }
}
