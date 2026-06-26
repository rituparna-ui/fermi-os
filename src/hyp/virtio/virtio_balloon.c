#include "virtio_balloon.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include <stdint.h>

/* --- virtio-mmio register offsets (modern / Version 2; same as rng/blk) ---- */
#define R_MAGIC          0x000 /* RO 0x74726976 "virt" */
#define R_VERSION        0x004 /* RO 2 */
#define R_DEVICE_ID      0x008 /* RO 5 = balloon */
#define R_VENDOR_ID      0x00C /* RO */
#define R_DEVICE_FEAT    0x010 /* RO, windowed by sel */
#define R_DEVICE_FEAT_SEL 0x014 /* WO */
#define R_DRIVER_FEAT    0x020 /* WO, windowed by sel */
#define R_DRIVER_FEAT_SEL 0x024 /* WO */
#define R_QUEUE_SEL      0x030 /* WO */
#define R_QUEUE_NUM_MAX  0x034 /* RO */
#define R_QUEUE_NUM      0x038 /* WO */
#define R_QUEUE_READY    0x044 /* RW */
#define R_QUEUE_NOTIFY   0x050 /* WO */
#define R_INT_STATUS     0x060 /* RO: bit0 used-buffer, bit1 config-change */
#define R_INT_ACK        0x064 /* WO */
#define R_STATUS         0x070 /* RW status bitfield */
#define R_QUEUE_DESC_LO  0x080
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_DRIVER_LO 0x090
#define R_QUEUE_DRIVER_HI 0x094
#define R_QUEUE_DEVICE_LO 0x0A0
#define R_QUEUE_DEVICE_HI 0x0A4
#define R_CONFIG_GEN     0x0FC /* RO: ConfigGeneration */
#define R_CONFIG         0x100 /* device config: num_pages(le32 @0) actual(le32 @4) */

#define MAGIC_VALUE 0x74726976UL
#define DEVICE_ID_BALLOON 5
#define VENDOR_ID 0x494D5246UL /* "FRMI" */
#define QUEUE_NUM_MAX 64
#define NQUEUES 2
#define BALLOON_INFLATEQ 0
#define BALLOON_DEFLATEQ 1

/* Status bits. */
#define ST_ACKNOWLEDGE 1
#define ST_DRIVER      2
#define ST_DRIVER_OK   4
#define ST_FEATURES_OK 8

/* InterruptStatus bits. */
#define INT_VRING  0x1 /* a used buffer was posted */
#define INT_CONFIG 0x2 /* device config (num_pages) changed */

/* Split-virtqueue descriptor flags. */
#define DESC_F_NEXT  0x1
#define DESC_F_WRITE 0x2

/* Self-driving autopilot: toggle num_pages between this goal and 0. */
#define BALLOON_INFLATE_GOAL   1024         /* 1024 pages = 4 MiB target */
#define BALLOON_RETARGET_TICKS 0x08000000ULL /* ~a few seconds at QEMU rates */

/* virtq_desc is 16 bytes: le64 addr, le32 len, le16 flags, le16 next. */
typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed)) virtq_desc_t;

/* One split virtqueue. */
typedef struct {
  uint32_t num, ready;
  uint64_t desc_ipa, driver_ipa, device_ipa; /* assembled from Lo/Hi writes */
  uint16_t last_avail_idx, used_idx;
} vq_t;

static struct {
  uint32_t status;
  uint32_t dev_feat_sel, drv_feat_sel;
  uint32_t int_status;
  uint32_t queue_sel;
  vq_t     q[NQUEUES]; /* q[0]=inflateq, q[1]=deflateq */

  /* Device config space. */
  uint32_t num_pages;  /* RO to guest: device's target balloon size */
  uint32_t actual;     /* RW: pages the driver reports in the balloon */
  uint32_t config_gen; /* bumped on every num_pages change */

  /* Honest bookkeeping. */
  uint32_t inflated_pages; /* count of pages currently donated (zeroed) */

  /* Self-driving auto-retarget. */
  uint64_t next_retarget_pct; /* CNTPCT deadline for the next num_pages flip */
  uint32_t inflate_goal;      /* the non-zero target we toggle to */

  int owner_id; /* vcpu id bound on first touch (-1 = unbound; id 0 is valid) */
} vbln;

/* Read `len` bytes of guest memory at IPA into `dst`, inval-first so EL2 (MMU
 * off, Non-cacheable) snoops the cacheable guest's latest writes. 0 on bad IPA. */
static int gread(uint64_t ipa, void *dst, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) {
    return 0;
  }
  hyp_dcache_inval_range(pa, len);
  const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)pa;
  uint8_t *d = (uint8_t *)dst;
  for (uint64_t i = 0; i < len; i++) d[i] = s[i];
  return 1;
}

/* Write `len` bytes to guest memory at IPA, then clean to PoC. 0 on bad IPA. */
static int gwrite(uint64_t ipa, const void *src, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) {
    return 0;
  }
  volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
  const uint8_t *s = (const uint8_t *)src;
  for (uint64_t i = 0; i < len; i++) d[i] = s[i];
  hyp_dcache_clean_range(pa, len);
  return 1;
}

/* Zero `len` bytes of guest memory at IPA, coherently. Bounds-checked: a PFN
 * pointing outside this VM's RAM yields pa==0 and is a no-op (VM-escape
 * defense). Operates on the returned host PA, never the raw IPA. */
static int gzero(uint64_t ipa, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) {
    return 0;
  }
  volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
  for (uint64_t i = 0; i < len; i++) d[i] = 0;
  hyp_dcache_clean_range(pa, len); /* push zeros to PoC for the cacheable guest */
  return 1;
}

/* Service an inflateq (qidx 0) or deflateq (qidx 1) notify. Buffers are device-
 * READ arrays of le32 PFNs (no DESC_F_WRITE); the device only reads them and
 * posts a used element with len=0. Inflate zeroes + counts each donated page;
 * deflate just decrements the counter (bookkeeping; no memory touch). */
static void balloon_process_queue(int qidx) {
  vq_t *q = &vbln.q[qidx];
  if (!(vbln.status & ST_DRIVER_OK) || !q->ready || q->num == 0) {
    return;
  }
  uint32_t n = q->num;
  if (n > QUEUE_NUM_MAX || (n & (n - 1)) != 0) {
    return; /* must be a power of two <= max */
  }

  uint16_t avail_idx;
  if (!gread(q->driver_ipa + 2, &avail_idx, 2)) {
    return;
  }

  int completed = 0;
  uint32_t pages_this_notify = 0;
  /* Hard per-notify page budget: caps TOTAL zeroing work regardless of n, chain
   * length, or per-descriptor count, so a hostile driver cannot make EL2 zero
   * ~1M pages in one trap. */
  const uint32_t PAGE_BUDGET = VIRTIO_BALLOON_PFNS_MAX;

  for (uint32_t guard = 0; guard < n; guard++) {
    if ((uint16_t)(avail_idx - q->last_avail_idx) == 0) {
      break; /* caught up */
    }
    if (pages_this_notify >= PAGE_BUDGET) {
      break; /* budget spent */
    }

    uint16_t slot = (uint16_t)(q->last_avail_idx % n);
    uint16_t head;
    if (!gread(q->driver_ipa + 4 + 2 * slot, &head, 2)) {
      break;
    }
    if (head >= n) {
      break; /* malformed */
    }

    /* Walk the chain; each descriptor's data is an array of le32 PFNs. */
    uint16_t di = head;
    for (uint32_t step = 0; step < n; step++) {
      virtq_desc_t d;
      if (!gread(q->desc_ipa + 16ULL * di, &d, sizeof(d))) {
        break;
      }

      /* Reject a malformed PFN buffer rather than silently clamping it. */
      if (d.flags & DESC_F_WRITE) {
        break; /* PFN arrays are device-READ; a WRITE desc is a protocol bug */
      }
      if (d.len & 3) {
        break; /* not a u32 array */
      }
      uint32_t count = d.len / 4;
      if (count > VIRTIO_BALLOON_PFNS_MAX) {
        break; /* oversized -> driver bug -> stop the chain */
      }

      /* Snapshot the WHOLE PFN array once: one dc-civac + one copy per
       * descriptor (not 256), and PFN values are captured BEFORE any gzero, so
       * a guest that aims a PFN at its own PFN-array page can't change values
       * mid-walk (TOCTOU closed). */
      uint32_t pfns[VIRTIO_BALLOON_PFNS_MAX];
      if (count && !gread(d.addr, pfns, (uint64_t)count * 4)) {
        break;
      }

      for (uint32_t i = 0; i < count; i++) {
        if (pages_this_notify >= PAGE_BUDGET) {
          break;
        }
        /* Cast to u64 BEFORE the shift: max 0xFFFFFFFF << 12 = 0xFFFFFFFFF000,
         * no 32-bit wrap. */
        uint64_t page_ipa = (uint64_t)pfns[i] << VIRTIO_BALLOON_PFN_SHIFT;

        if (qidx == BALLOON_INFLATEQ) {
          /* INFLATE: donate -> zero the page. gzero/vcpu_ipa_to_pa confine
           * page_ipa wholly to [entry_ipa, entry_ipa+ram_size): a PFN at the
           * MMIO window / another VM / the hyp yields pa==0 and is skipped. */
          if (gzero(page_ipa, 4096)) {
            vbln.inflated_pages++;
            pages_this_notify++;
          }
        } else {
          /* DEFLATE: return -> bookkeeping only (still bounds-check the PFN). */
          if (vcpu_ipa_to_pa(cur_vcpu, page_ipa, 4096)) {
            if (vbln.inflated_pages) {
              vbln.inflated_pages--;
            } else {
              hyp_puts("[VBALLOON] WARN: deflate underflow (driver bug)\n");
            }
            pages_this_notify++;
          }
        }
      }

      if (!(d.flags & DESC_F_NEXT)) {
        break;
      }
      if (d.next >= n) {
        break;
      }
      di = d.next;
    }

    /* Post used element { id=head, len=0 } (device wrote nothing into guest). */
    uint32_t elem[2] = { head, 0 };
    gwrite(q->device_ipa + 4 + 8ULL * (q->used_idx % n), elem, sizeof(elem));
    q->used_idx++;
    q->last_avail_idx++;
    completed++;
  }

  if (completed) {
    /* Strict publish ordering (mirrors rng): elements cleaned above, barrier,
     * publish used.idx, barrier, then raise + inject. */
    __asm__ __volatile__("dsb ish" ::: "memory");
    gwrite(q->device_ipa + 2, &q->used_idx, 2);
    __asm__ __volatile__("dsb ish" ::: "memory");

    vbln.int_status |= INT_VRING;
    /* Owner-guard: vgic_inject_ppi targets the LIVE List Register of cur_vcpu,
     * correct only when cur_vcpu IS the owner. It always is here (only the
     * owner's stage-2 traps this window), but guard explicitly. */
    if (cur_vcpu && (int)cur_vcpu->id == vbln.owner_id) {
      vgic_inject_ppi(VIRTIO_BALLOON_SPI);
    }

    hyp_puts(qidx == BALLOON_INFLATEQ ? "[VBALLOON] inflate: zeroed "
                                      : "[VBALLOON] deflate: returned ");
    hyp_puthex(pages_this_notify);
    hyp_puts(qidx == BALLOON_INFLATEQ
                 ? " pages (NOT host-unmapped; fixed stage-2). balloon="
                 : " pages. balloon=");
    hyp_puthex(vbln.inflated_pages);
    hyp_puts(" pages\n");
  }
}

/* Self-driving config clock. Called ONLY from write-side traps (NOTIFY/INT_ACK/
 * STATUS), NEVER on a register read — so config_gen/num_pages can never mutate
 * during the driver's (read gen, read num_pages, re-read gen) snapshot, making
 * ConfigGeneration structurally consistent. Toggles num_pages goal<->0 and
 * raises a config-change interrupt each deadline. */
static void balloon_autopilot(void) {
  if (!(vbln.status & ST_DRIVER_OK)) {
    return; /* wait until the driver is fully up */
  }
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  if (vbln.next_retarget_pct != 0 && now < vbln.next_retarget_pct) {
    return; /* not yet */
  }

  vbln.next_retarget_pct = now + BALLOON_RETARGET_TICKS;
  if (vbln.num_pages == 0) {
    vbln.num_pages = vbln.inflate_goal; /* inflate target */
  } else {
    vbln.num_pages = 0; /* deflate to zero */
  }
  vbln.config_gen++; /* bump BEFORE raising INT_CONFIG (read protocol relies on it) */
  vbln.int_status |= INT_CONFIG;
  /* Make the config + status stores visible before the IRQ, mirroring the vring
   * publish discipline, so a relaxed core can't deliver the IRQ early. */
  __asm__ __volatile__("dsb ish" ::: "memory");
  if (cur_vcpu && (int)cur_vcpu->id == vbln.owner_id) {
    vgic_inject_ppi(VIRTIO_BALLOON_SPI);
  }

  hyp_puts("[VBALLOON] config-change: num_pages -> ");
  hyp_puthex(vbln.num_pages);
  hyp_puts(vbln.num_pages ? " (inflate target)\n" : " (deflate to zero)\n");
}

int virtio_balloon_mmio_is_target(uint64_t ipa) {
  return ipa >= VIRTIO_BALLOON_MMIO_BASE &&
         ipa < VIRTIO_BALLOON_MMIO_BASE + VIRTIO_BALLOON_MMIO_SIZE;
}

void virtio_balloon_init(void) {
  vbln.owner_id = -1; /* unbound (vcpu id 0 is valid, so 0 can't mean unbound) */
  vbln.inflate_goal = BALLOON_INFLATE_GOAL;
  hyp_puts("[VBALLOON] device ready (DeviceID 5; honest model: inflate=zero, no host unmap)\n");
}

void virtio_balloon_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                                 int size_bytes) {
  (void)size_bytes; /* virtio-mmio registers are 32-bit word accesses */
  uint64_t off = ipa - VIRTIO_BALLOON_MMIO_BASE;

  if (vbln.owner_id < 0 && cur_vcpu) {
    vbln.owner_id = (int)cur_vcpu->id; /* bind to first toucher */
  }

  if (!is_write) {
    uint32_t r = 0;
    switch (off) {
    case R_MAGIC:         r = MAGIC_VALUE; break;
    case R_VERSION:       r = 2; break;
    case R_DEVICE_ID:     r = DEVICE_ID_BALLOON; break;
    case R_VENDOR_ID:     r = VENDOR_ID; break;
    case R_DEVICE_FEAT:   r = (vbln.dev_feat_sel == 1) ? 0x1u : 0x0u; break; /* VERSION_1 only */
    case R_QUEUE_NUM_MAX: r = (vbln.queue_sel < NQUEUES) ? QUEUE_NUM_MAX : 0; break;
    case R_QUEUE_READY:   r = vbln.q[vbln.queue_sel & 1].ready; break;
    case R_INT_STATUS:    r = vbln.int_status; break;
    case R_STATUS:        r = vbln.status; break;
    case R_CONFIG_GEN:    r = vbln.config_gen; break; /* NO autopilot on a read */
    case R_CONFIG + 0:    r = vbln.num_pages; break;  /* NO autopilot on a read */
    case R_CONFIG + 4:    r = vbln.actual; break;
    default:              r = 0; break;
    }
    *val = r;
    return;
  }

  /* Write. */
  uint32_t w = (uint32_t)*val;
  vq_t *q = &vbln.q[vbln.queue_sel & 1];
  switch (off) {
  case R_DEVICE_FEAT_SEL: vbln.dev_feat_sel = w; break;
  case R_DRIVER_FEAT_SEL: vbln.drv_feat_sel = w; break;
  case R_DRIVER_FEAT:     /* accept; only VERSION_1 offered, no validation */ break;
  case R_QUEUE_SEL:       vbln.queue_sel = w; break;
  case R_QUEUE_NUM:       q->num = w; break;
  case R_QUEUE_READY:
    q->ready = w & 1;
    if (!q->ready) { q->last_avail_idx = 0; q->used_idx = 0; }
    break;
  case R_QUEUE_DESC_LO:   q->desc_ipa = (q->desc_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DESC_HI:   q->desc_ipa = (q->desc_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DRIVER_LO: q->driver_ipa = (q->driver_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DRIVER_HI: q->driver_ipa = (q->driver_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DEVICE_LO: q->device_ipa = (q->device_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DEVICE_HI: q->device_ipa = (q->device_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_NOTIFY:
    balloon_autopilot(); /* write-side clock */
    if (w == BALLOON_INFLATEQ)      balloon_process_queue(BALLOON_INFLATEQ);
    else if (w == BALLOON_DEFLATEQ) balloon_process_queue(BALLOON_DEFLATEQ);
    break;
  case R_INT_ACK:
    vbln.int_status &= ~w;
    balloon_autopilot(); /* write-side clock */
    break;
  case R_CONFIG + 0: /* num_pages is RO to the guest — ignore */ break;
  case R_CONFIG + 4:
    vbln.actual = w;
    hyp_puts("[VBALLOON] driver reports actual=");
    hyp_puthex(w);
    hyp_puts(" pages\n");
    break;
  case R_STATUS:
    vbln.status = w;
    if (w == 0) { /* full reset (clears config + autopilot too) */
      vbln.int_status = 0;
      vbln.num_pages = 0;
      vbln.actual = 0;
      vbln.config_gen = 0;
      vbln.inflated_pages = 0;
      vbln.next_retarget_pct = 0;
      for (int i = 0; i < NQUEUES; i++) {
        vbln.q[i].ready = vbln.q[i].num = 0;
        vbln.q[i].last_avail_idx = vbln.q[i].used_idx = 0;
        vbln.q[i].desc_ipa = vbln.q[i].driver_ipa = vbln.q[i].device_ipa = 0;
      }
    } else {
      balloon_autopilot(); /* write-side clock (e.g. on DRIVER_OK) */
    }
    break;
  default: break;
  }
}
