#include "virtio_blk.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include "hyp_alloc.h"
#include <stdint.h>

/* virtio-mmio register offsets (same modern layout as the RNG device). */
#define R_MAGIC          0x000
#define R_VERSION        0x004
#define R_DEVICE_ID      0x008
#define R_VENDOR_ID      0x00C
#define R_DEVICE_FEAT    0x010
#define R_DEVICE_FEAT_SEL 0x014
#define R_DRIVER_FEAT    0x020
#define R_DRIVER_FEAT_SEL 0x024
#define R_QUEUE_SEL      0x030
#define R_QUEUE_NUM_MAX  0x034
#define R_QUEUE_NUM      0x038
#define R_QUEUE_READY    0x044
#define R_QUEUE_NOTIFY   0x050
#define R_INT_STATUS     0x060
#define R_INT_ACK        0x064
#define R_STATUS         0x070
#define R_QUEUE_DESC_LO  0x080
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_DRIVER_LO 0x090
#define R_QUEUE_DRIVER_HI 0x094
#define R_QUEUE_DEVICE_LO 0x0A0
#define R_QUEUE_DEVICE_HI 0x0A4
#define R_CONFIG_GEN     0x0FC
#define R_CONFIG         0x100 /* device-config region: blk capacity (le64) */

#define MAGIC_VALUE 0x74726976UL
#define DEVICE_ID_BLOCK 2
#define VENDOR_ID 0x494D5246UL /* "FRMI" */
#define QUEUE_NUM_MAX 64

#define ST_DRIVER_OK   4
#define INT_VRING      0x1

#define DESC_F_NEXT  0x1
#define DESC_F_WRITE 0x2

/* virtio-blk request header (16 bytes): le32 type, le32 reserved, le64 sector. */
#define VIRTIO_BLK_T_IN  0   /* read disk -> guest */
#define VIRTIO_BLK_T_OUT 1   /* write guest -> disk */
#define VIRTIO_BLK_S_OK  0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} __attribute__((packed)) blk_req_hdr_t;

static struct {
  uint32_t status, dev_feat_sel, int_status;
  uint32_t queue_sel, queue_num, queue_ready;
  uint64_t desc_ipa, driver_ipa, device_ipa;
  uint16_t last_avail_idx, used_idx;
} vblk;

/* RAM-backed disk (carved from the hyp pool at boot). */
static uint8_t *disk;
static uint64_t disk_bytes;

void virtio_blk_init(void) {
  disk_bytes = (uint64_t)VIRTIO_BLK_NSECTORS * VIRTIO_BLK_SECTOR;
  uint64_t pages = (disk_bytes + HYP_PAGE_SIZE - 1) / HYP_PAGE_SIZE;
  disk = (uint8_t *)hyp_alloc_pages(pages);
  /* Stamp sector 0 so a guest read returns something recognizable. */
  const char *sig = "FERMI-VIRTIO-BLK disk sector 0\n";
  for (int i = 0; sig[i]; i++) disk[i] = (uint8_t)sig[i];
  hyp_puts("[VBLK] backing disk ready (");
  hyp_puthex(disk_bytes);
  hyp_puts(" bytes)\n");
}

/* Cache helpers around guest memory (EL2 MMU off vs cacheable guest). */
static int gread(uint64_t ipa, void *dst, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) return 0;
  hyp_dcache_inval_range(pa, len);
  const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)pa;
  uint8_t *d = (uint8_t *)dst;
  for (uint64_t i = 0; i < len; i++) d[i] = s[i];
  return 1;
}
static int gwrite(uint64_t ipa, const void *src, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) return 0;
  volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
  const uint8_t *s = (const uint8_t *)src;
  for (uint64_t i = 0; i < len; i++) d[i] = s[i];
  hyp_dcache_clean_range(pa, len);
  return 1;
}

/* Service one request chain starting at descriptor `head`. Returns total bytes
 * written into guest WRITE buffers (for the used-ring len field). */
static uint32_t blk_service_chain(uint16_t head, uint32_t n) {
  /* Descriptor 0: the request header (RO). */
  virtq_desc_t d0;
  if (!gread(vblk.desc_ipa + 16ULL * head, &d0, sizeof(d0))) return 0;
  blk_req_hdr_t hdr;
  if (d0.len < sizeof(hdr) || !gread(d0.addr, &hdr, sizeof(hdr))) return 0;
  if (!(d0.flags & DESC_F_NEXT)) return 0;

  uint64_t off = hdr.sector * VIRTIO_BLK_SECTOR; /* byte offset on disk */
  uint8_t status = VIRTIO_BLK_S_OK;
  uint32_t used_len = 0;

  /* Walk the middle data descriptors until the final (status) descriptor,
   * which is the WO 1-byte one with no NEXT. Bound the chain to n. */
  uint16_t di = d0.next;
  uint16_t status_desc = 0xFFFF;
  for (uint32_t step = 0; step < n; step++) {
    virtq_desc_t d;
    if (di >= n || !gread(vblk.desc_ipa + 16ULL * di, &d, sizeof(d))) {
      status = VIRTIO_BLK_S_IOERR; break;
    }
    int has_next = (d.flags & DESC_F_NEXT) != 0;
    if (!has_next) {
      /* Last descriptor = status byte (WO). */
      status_desc = di;
      break;
    }
    /* Data descriptor: WRITE => device fills (read req); else device reads
     * (write req). Clamp to the disk. */
    uint64_t len = d.len;
    if (off + len > disk_bytes) {
      status = VIRTIO_BLK_S_IOERR;
      /* still consume to find status desc */
    } else if (hdr.type == VIRTIO_BLK_T_IN && (d.flags & DESC_F_WRITE)) {
      gwrite(d.addr, disk + off, len);   /* disk -> guest */
      used_len += (uint32_t)len;
      off += len;
    } else if (hdr.type == VIRTIO_BLK_T_OUT && !(d.flags & DESC_F_WRITE)) {
      gread(d.addr, disk + off, len);    /* guest -> disk */
      off += len;
    } else if (hdr.type != VIRTIO_BLK_T_IN && hdr.type != VIRTIO_BLK_T_OUT) {
      status = VIRTIO_BLK_S_UNSUPP;
    }
    di = d.next;
  }

  /* Write the status byte into the final WO descriptor. */
  if (status_desc != 0xFFFF) {
    virtq_desc_t sd;
    if (gread(vblk.desc_ipa + 16ULL * status_desc, &sd, sizeof(sd)) &&
        sd.len >= 1) {
      gwrite(sd.addr, &status, 1);
      used_len += 1;
    }
  }
  return used_len;
}

static void virtio_blk_process_queue(void) {
  if (!(vblk.status & ST_DRIVER_OK) || !vblk.queue_ready || vblk.queue_num == 0)
    return;
  uint32_t n = vblk.queue_num;
  if (n > QUEUE_NUM_MAX || (n & (n - 1)) != 0) return;

  uint16_t avail_idx;
  if (!gread(vblk.driver_ipa + 2, &avail_idx, 2)) return;

  int completed = 0;
  for (uint32_t guard = 0; guard < n; guard++) {
    if ((uint16_t)(avail_idx - vblk.last_avail_idx) == 0) break;
    uint16_t slot = (uint16_t)(vblk.last_avail_idx % n);
    uint16_t head;
    if (!gread(vblk.driver_ipa + 4 + 2 * slot, &head, 2)) break;
    if (head >= n) break;

    uint32_t used_len = blk_service_chain(head, n);

    uint32_t elem[2] = { head, used_len };
    gwrite(vblk.device_ipa + 4 + 8ULL * (vblk.used_idx % n), elem, sizeof(elem));
    vblk.used_idx++;
    vblk.last_avail_idx++;
    completed++;
  }

  if (completed) {
    __asm__ __volatile__("dsb ish" ::: "memory");
    gwrite(vblk.device_ipa + 2, &vblk.used_idx, 2);
    __asm__ __volatile__("dsb ish" ::: "memory");
    vblk.int_status |= INT_VRING;
    vgic_inject_ppi(VIRTIO_BLK_SPI);
  }
}

int virtio_blk_mmio_is_target(uint64_t ipa) {
  return ipa >= VIRTIO_BLK_MMIO_BASE &&
         ipa < VIRTIO_BLK_MMIO_BASE + VIRTIO_BLK_MMIO_SIZE;
}

void virtio_blk_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                             int size_bytes) {
  (void)size_bytes;
  uint64_t off = ipa - VIRTIO_BLK_MMIO_BASE;

  if (!is_write) {
    uint32_t r = 0;
    switch (off) {
    case R_MAGIC:         r = MAGIC_VALUE; break;
    case R_VERSION:       r = 2; break;
    case R_DEVICE_ID:     r = DEVICE_ID_BLOCK; break;
    case R_VENDOR_ID:     r = VENDOR_ID; break;
    case R_DEVICE_FEAT:   r = (vblk.dev_feat_sel == 1) ? 0x1u : 0x0u; break;
    case R_QUEUE_NUM_MAX: r = (vblk.queue_sel == 0) ? QUEUE_NUM_MAX : 0; break;
    case R_QUEUE_READY:   r = vblk.queue_ready; break;
    case R_INT_STATUS:    r = vblk.int_status; break;
    case R_STATUS:        r = vblk.status; break;
    case R_CONFIG_GEN:    r = 0; break;
    /* blk config: capacity in 512-byte sectors (le64) at config offset 0. */
    case R_CONFIG + 0:    r = (uint32_t)(uint64_t)VIRTIO_BLK_NSECTORS; break;
    case R_CONFIG + 4:    r = (uint32_t)((uint64_t)VIRTIO_BLK_NSECTORS >> 32); break;
    default:              r = 0; break;
    }
    *val = r;
    return;
  }

  uint32_t w = (uint32_t)*val;
  switch (off) {
  case R_DEVICE_FEAT_SEL: vblk.dev_feat_sel = w; break;
  case R_DRIVER_FEAT_SEL: break;
  case R_DRIVER_FEAT:     break;
  case R_QUEUE_SEL:       vblk.queue_sel = w; break;
  case R_QUEUE_NUM:       vblk.queue_num = w; break;
  case R_QUEUE_READY:
    vblk.queue_ready = w & 1;
    if (!vblk.queue_ready) { vblk.last_avail_idx = 0; vblk.used_idx = 0; }
    break;
  case R_QUEUE_DESC_LO:   vblk.desc_ipa = (vblk.desc_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DESC_HI:   vblk.desc_ipa = (vblk.desc_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DRIVER_LO: vblk.driver_ipa = (vblk.driver_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DRIVER_HI: vblk.driver_ipa = (vblk.driver_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DEVICE_LO: vblk.device_ipa = (vblk.device_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DEVICE_HI: vblk.device_ipa = (vblk.device_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_NOTIFY:    if (w == 0) virtio_blk_process_queue(); break;
  case R_INT_ACK:         vblk.int_status &= ~w; break;
  case R_STATUS:
    vblk.status = w;
    if (w == 0) {
      vblk.queue_ready = 0; vblk.queue_num = 0; vblk.int_status = 0;
      vblk.last_avail_idx = 0; vblk.used_idx = 0;
      vblk.desc_ipa = vblk.driver_ipa = vblk.device_ipa = 0;
    }
    break;
  default: break;
  }
}
