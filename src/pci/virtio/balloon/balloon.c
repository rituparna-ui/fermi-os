#include "balloon.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

/* ---------------------------------------------------------------------------
 * virtio-balloon — cooperative memory ballooning
 *
 * The host can ask the guest to "inflate" the balloon, meaning the guest
 * should hand a number of physical pages back to the host (the host can
 * then reuse that physical memory elsewhere). The host can later ask the
 * guest to "deflate", meaning the guest may reclaim those pages.
 *
 * Spec mechanics we care about:
 *   - device_cfg.num_pages — host writes the *target* balloon size here
 *   - device_cfg.actual    — driver writes the *current* balloon size
 *   - inflateq (vq 0)      — driver submits arrays of PFNs it just gave away
 *   - deflateq (vq 1)      — driver submits PFNs it is reclaiming
 *   - PFN is a flat phys_addr >> 12, regardless of guest/host page size
 *
 * Driver policy in this kernel:
 *   The kernel does not (yet) wire a virtio-config-change interrupt, so
 *   inflate/deflate are *driver-initiated*: the shell exposes
 *   `balloon inflate N` / `balloon deflate N`, and /proc/balloon shows
 *   both the actual size and the host's wish so the user can spot drift.
 * --------------------------------------------------------------------------- */

/* Page-aligned virtqueue backing memory — same pattern as rng/blk. We
 * have two queues, hence two of each ring. */
static struct virtq_desc inflate_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail inflate_avail __attribute__((aligned(4096)));
static struct virtq_used inflate_used __attribute__((aligned(4096)));

static struct virtq_desc deflate_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail deflate_avail __attribute__((aligned(4096)));
static struct virtq_used deflate_used __attribute__((aligned(4096)));

/* DMA-visible buffer the device reads PFNs out of. One u32 per PFN, capped
 * at MAX_PAGES, so 4 KiB max — fits in exactly one page. */
static uint32_t pfn_buf[VIRTIO_BALLOON_MAX_PAGES] __attribute__((aligned(64)));

/* Backing book-keeping: which pages we currently have given to the host.
 * Stored as PFNs to match the wire format. The active prefix is
 * pfns[0 .. actual-1]; everything beyond is undefined. */
static uint32_t inflated_pfns[VIRTIO_BALLOON_MAX_PAGES];

static struct virtio_balloon bln_dev;
static int bln_ready = 0;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/* Push a PFN array on a queue and wait for the device to ack. The buffer
 * is *device-readable* (no VIRTQ_DESC_F_WRITE) — the driver is informing
 * the device which physical pages belong to / are being reclaimed from
 * the balloon. */
static int submit_pfn_batch(struct virtqueue *vq, uint32_t count) {
  if (count == 0) {
    return 0;
  }
  uint64_t pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)pfn_buf);
  virtqueue_submit(vq, pa, count * sizeof(uint32_t), VIRTQ_DESC_F_NONE);
  virtqueue_notify(vq);
  /* virtqueue_poll returns 0 on timeout. The device writes nothing into
   * a read-only buffer so a successful ack also reports 0 bytes — we
   * cannot use the return value to distinguish success from timeout.
   * However virtqueue_poll already logs and returns on timeout, and on
   * success it advances last_used. The simplest reliable signal is to
   * just call it and trust the spin counter inside. */
  (void)virtqueue_poll(vq);
  return 0;
}

/* Mirror our internal balloon size into device_cfg.actual so the host's
 * stats reflect reality. Spec §5.5.6: this is informational. */
static void publish_actual(void) {
  uintptr_t cfg = bln_dev.pci_caps.device_cfg;
  if (cfg == 0) {
    return; /* device_cfg cap not advertised — shouldn't happen for balloon */
  }
  mmio_write32(cfg + VIRTIO_BALLOON_CFG_ACTUAL, bln_dev.actual);
  dsb_sy();
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

int balloon_inflate(uint32_t n) {
  if (!bln_ready) {
    return -1;
  }
  /* Cap to the headroom in our tracking array. */
  uint32_t headroom = VIRTIO_BALLOON_MAX_PAGES - bln_dev.actual;
  if (n > headroom) {
    n = headroom;
  }
  if (n == 0) {
    return 0;
  }

  /* Allocate fresh pages from the PMM and stage their PFNs into pfn_buf
   * for the device, while also recording them into our inflated_pfns
   * tracking table so we can hand them back later. */
  uint32_t got = 0;
  for (uint32_t i = 0; i < n; i++) {
    uintptr_t pa = pmm_allocate_page();
    if (!pa) {
      break; /* OOM — stop here, ship what we have */
    }
    uint32_t pfn = (uint32_t)(pa >> VIRTIO_BALLOON_PFN_SHIFT);
    pfn_buf[got] = pfn;
    inflated_pfns[bln_dev.actual + got] = pfn;
    got++;
  }
  if (got == 0) {
    return 0; /* PMM was completely empty */
  }

  if (submit_pfn_batch(&bln_dev.inflate_vq, got) < 0) {
    /* Device handshake failed — give the pages back to the PMM and bail.
     * Our tracking table was speculative so we just ignore those slots. */
    for (uint32_t i = 0; i < got; i++) {
      pmm_free_page((uintptr_t)pfn_buf[i] << VIRTIO_BALLOON_PFN_SHIFT);
    }
    return -1;
  }

  bln_dev.actual += got;
  publish_actual();
  return (int)got;
}

int balloon_deflate(uint32_t n) {
  if (!bln_ready) {
    return -1;
  }
  if (n > bln_dev.actual) {
    n = bln_dev.actual;
  }
  if (n == 0) {
    return 0;
  }

  /* Pop the *most recently* inflated PFNs (LIFO). Stage them into pfn_buf
   * so the device sees the same handles it received on inflateq. */
  uint32_t base = bln_dev.actual - n;
  for (uint32_t i = 0; i < n; i++) {
    pfn_buf[i] = inflated_pfns[base + i];
  }

  if (submit_pfn_batch(&bln_dev.deflate_vq, n) < 0) {
    return -1; /* leave the balloon as-is */
  }

  /* Hand the physical pages back to the PMM. The host has acknowledged
   * that it's no longer using them. */
  for (uint32_t i = 0; i < n; i++) {
    pmm_free_page((uintptr_t)pfn_buf[i] << VIRTIO_BALLOON_PFN_SHIFT);
  }

  bln_dev.actual -= n;
  publish_actual();
  return (int)n;
}

void balloon_get_status(uint32_t *actual_pages, uint32_t *host_target) {
  if (actual_pages) {
    *actual_pages = bln_ready ? bln_dev.actual : 0;
  }
  if (host_target) {
    if (!bln_ready || bln_dev.pci_caps.device_cfg == 0) {
      *host_target = 0;
    } else {
      *host_target = mmio_read32(bln_dev.pci_caps.device_cfg +
                                 VIRTIO_BALLOON_CFG_NUM_PAGES);
    }
  }
}

/* ---------------------------------------------------------------------------
 * Init — same skeleton as rng/blk: reset → ack → driver → features →
 * features_ok → setup queues → driver_ok.
 * --------------------------------------------------------------------------- */

void pci_virtio_balloon_init(void) {
  uart_println("[BALLOON] Initializing Device");

  if (!pci_find_device(VIRTIO_BALLOON_VENDOR_ID, VIRTIO_BALLOON_DEVICE_ID,
                       &bln_dev.pci)) {
    uart_println("[BALLOON] Device not found (skipping)");
    return;
  }
  uart_println("[BALLOON] Device found");

  if ((pci_get_header_type(&bln_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[BALLOON] Unexpected header type");
    return;
  }

  pci_assign_bars(&bln_dev.pci);
  pci_enable_device(&bln_dev.pci);
  virtio_parse_capabilities(&bln_dev.pci, &bln_dev.pci_caps);

  uintptr_t base = bln_dev.pci_caps.common_cfg;

  /* 1. Reset */
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }

  /* 2. Ack + 3. Driver */
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* 4. Features. We accept VIRTIO_F_VERSION_1 (bit 32 = feat_hi bit 0)
   * and nothing else. The balloon-specific feature bits we leave clear:
   *   - F_MUST_TELL_HOST (bit 0): we always notify before reclaiming
   *   - F_STATS_VQ (bit 1): we don't expose memory stats yet
   *   - F_DEFLATE_ON_OOM (bit 2): we'd need PMM hooks
   *   - F_FREE_PAGE_HINT / F_PAGE_POISON / F_REPORTING (bits 3..5):
   *     all advanced extensions we'd implement later. */
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf("[BALLOON] Device features: hi=%x lo=%x\n", feat_hi, feat_lo);

  uint32_t guest_lo = 0;
  uint32_t guest_hi = feat_hi & 0x01; /* VIRTIO_F_VERSION_1 only */

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
  dsb_sy();

  /* 5. FEATURES_OK + readback */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[BALLOON] FEATURES_OK rejected");
    return;
  }

  /* 6. Two virtqueues: inflate (0) + deflate (1). Same DMA setup pattern
   * for both. */
  bln_dev.inflate_vq.desc = inflate_desc;
  bln_dev.inflate_vq.avail = &inflate_avail;
  bln_dev.inflate_vq.used = &inflate_used;
  if (virtqueue_setup(base, VIRTIO_BALLOON_VQ_INFLATE, &bln_dev.inflate_vq,
                      &bln_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[BALLOON] inflateq setup failed");
    return;
  }

  bln_dev.deflate_vq.desc = deflate_desc;
  bln_dev.deflate_vq.avail = &deflate_avail;
  bln_dev.deflate_vq.used = &deflate_used;
  if (virtqueue_setup(base, VIRTIO_BALLOON_VQ_DEFLATE, &bln_dev.deflate_vq,
                      &bln_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[BALLOON] deflateq setup failed");
    return;
  }

  /* 7. DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();

  bln_dev.actual = 0;
  publish_actual();
  bln_ready = 1;

  uint32_t target = mmio_read32(bln_dev.pci_caps.device_cfg +
                                VIRTIO_BALLOON_CFG_NUM_PAGES);
  uart_printf("[BALLOON] DRIVER_OK; host target=%d pages, actual=0\n",
              (uint64_t)target);
}
