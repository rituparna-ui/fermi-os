#include "virtio_rng.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include "hyp_alloc.h"
#include <stdint.h>

/* --- virtio-mmio register offsets (modern / Version 2) --------------------- */
#define R_MAGIC          0x000 /* RO 0x74726976 "virt" */
#define R_VERSION        0x004 /* RO 2 */
#define R_DEVICE_ID      0x008 /* RO 4 = entropy */
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
#define R_INT_STATUS     0x060 /* RO: bit0 used-buffer */
#define R_INT_ACK        0x064 /* WO */
#define R_STATUS         0x070 /* RW status bitfield */
#define R_QUEUE_DESC_LO  0x080
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_DRIVER_LO 0x090
#define R_QUEUE_DRIVER_HI 0x094
#define R_QUEUE_DEVICE_LO 0x0A0
#define R_QUEUE_DEVICE_HI 0x0A4
#define R_CONFIG_GEN     0x0FC /* RO 0 */

#define MAGIC_VALUE 0x74726976UL
#define DEVICE_ID_ENTROPY 4
#define VENDOR_ID 0x494D5246UL /* "FRMI" */
#define QUEUE_NUM_MAX 64

/* Status bits. */
#define ST_ACKNOWLEDGE 1
#define ST_DRIVER      2
#define ST_DRIVER_OK   4
#define ST_FEATURES_OK 8

/* InterruptStatus bits. */
#define INT_VRING 0x1

/* Split-virtqueue descriptor flags. */
#define DESC_F_NEXT  0x1
#define DESC_F_WRITE 0x2

/* virtq_desc is 16 bytes: le64 addr, le32 len, le16 flags, le16 next. */
typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed)) virtq_desc_t;

/* Per-device state (single device, single queue 0). */
static struct {
  uint32_t status;
  uint32_t dev_feat_sel;
  uint32_t drv_feat_sel;
  uint32_t int_status;
  uint32_t queue_sel;
  uint32_t queue_num;
  uint32_t queue_ready;
  /* Three ring base IPAs, assembled from the Lo/Hi register writes. */
  uint64_t desc_ipa;
  uint64_t driver_ipa; /* avail ring */
  uint64_t device_ipa; /* used ring  */
  uint16_t last_avail_idx;
  uint16_t used_idx;
  int      owner_id;   /* vcpu id that owns this device (set on first touch) */
} vrng;

/* xorshift64 PRNG seeded from CNTPCT. NOT cryptographic — a transport demo. */
static uint64_t prng_state;
static uint8_t prng_byte(void) {
  if (prng_state == 0) {
    uint64_t t;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(t));
    prng_state = t | 1;
  }
  uint64_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  prng_state = x;
  return (uint8_t)(x >> 24);
}

/* Read `len` bytes of guest memory at IPA `ipa` into `dst`, with a clean+inval
 * first so EL2 (MMU off) sees the cacheable guest's latest writes. Returns 0 on
 * a bad (out-of-RAM) IPA. */
static int guest_read(uint64_t ipa, void *dst, uint64_t len) {
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

/* Write `len` bytes to guest memory at IPA `ipa`, then clean to PoC so the
 * cacheable guest observes them. Returns 0 on a bad IPA. */
static int guest_write(uint64_t ipa, const void *src, uint64_t len) {
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

/* Fill `len` bytes of guest buffer at IPA `ipa` with PRNG bytes. */
static int fill_random(uint64_t ipa, uint64_t len) {
  uint64_t pa = vcpu_ipa_to_pa(cur_vcpu, ipa, len);
  if (!pa) {
    return 0;
  }
  volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
  for (uint64_t i = 0; i < len; i++) d[i] = prng_byte();
  hyp_dcache_clean_range(pa, len);
  return 1;
}

/* Service a QueueNotify: walk the avail ring, fill WRITE descriptors, post used
 * elements, advance used.idx, raise the interrupt. */
static void virtio_process_queue(void) {
  if (!(vrng.status & ST_DRIVER_OK) || !vrng.queue_ready ||
      vrng.queue_num == 0) {
    return;
  }
  uint32_t n = vrng.queue_num;
  if (n > QUEUE_NUM_MAX || (n & (n - 1)) != 0) {
    return; /* must be a power of two <= max */
  }

  /* Read the live avail.idx (at driver_ipa + offsetof(idx) = +2). */
  uint16_t avail_idx;
  if (!guest_read(vrng.driver_ipa + 2, &avail_idx, 2)) {
    return;
  }

  int completed = 0;
  /* Process every newly-available head, bounded to n entries per notify so a
   * malicious avail.idx can't spin EL2 unbounded. */
  for (uint32_t guard = 0; guard < n; guard++) {
    if ((uint16_t)(avail_idx - vrng.last_avail_idx) == 0) {
      break; /* caught up */
    }
    /* avail.ring[i % n] at driver_ipa + 4 + 2*(i%n). */
    uint16_t slot = (uint16_t)(vrng.last_avail_idx % n);
    uint16_t head;
    if (!guest_read(vrng.driver_ipa + 4 + 2 * slot, &head, 2)) {
      break;
    }
    if (head >= n) {
      break; /* malformed */
    }

    /* Walk the descriptor chain, filling WRITE buffers. Bound chain to n. */
    uint32_t total = 0;
    uint16_t di = head;
    for (uint32_t step = 0; step < n; step++) {
      virtq_desc_t d;
      if (!guest_read(vrng.desc_ipa + 16ULL * di, &d, sizeof(d))) {
        break;
      }
      if (d.flags & DESC_F_WRITE) {
        if (fill_random(d.addr, d.len)) {
          total += d.len;
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

    /* Post used element { id=head, len=total } at used.ring[used_idx % n].
     * used ring: flags(2) idx(2) then ring[]; each elem is 8 bytes. */
    uint32_t elem[2] = { head, total };
    uint64_t elem_ipa = vrng.device_ipa + 4 + 8ULL * (vrng.used_idx % n);
    guest_write(elem_ipa, elem, sizeof(elem)); /* cleans elem to PoC */

    vrng.used_idx++;
    vrng.last_avail_idx++;
    completed++;
  }

  if (completed) {
    /* Strict ordering: elements are already cleaned above. Barrier, THEN
     * publish the new used.idx (at device_ipa + 2), then barrier again so the
     * idx is visible before the IRQ. guest_write cleans the idx to PoC. */
    __asm__ __volatile__("dsb ish" ::: "memory");
    guest_write(vrng.device_ipa + 2, &vrng.used_idx, 2);
    __asm__ __volatile__("dsb ish" ::: "memory");

    vrng.int_status |= INT_VRING;
    /* Inject the device SPI into the owner. cur_vcpu IS the owner (only its
     * stage-2 traps this window), so use the live List Register path. */
    vgic_inject_ppi(VIRTIO_RNG_SPI);
  }
}

int virtio_mmio_is_target(uint64_t ipa) {
  return ipa >= VIRTIO_MMIO_BASE && ipa < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE;
}

void virtio_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                         int size_bytes) {
  (void)size_bytes; /* virtio-mmio registers are 32-bit word accesses */
  uint64_t off = ipa - VIRTIO_MMIO_BASE;

  if (vrng.owner_id == 0 && cur_vcpu) {
    vrng.owner_id = (int)cur_vcpu->id; /* bind to first toucher */
  }

  if (!is_write) {
    uint32_t r = 0;
    switch (off) {
    case R_MAGIC:         r = MAGIC_VALUE; break;
    case R_VERSION:       r = 2; break;
    case R_DEVICE_ID:     r = DEVICE_ID_ENTROPY; break;
    case R_VENDOR_ID:     r = VENDOR_ID; break;
    case R_DEVICE_FEAT:   r = (vrng.dev_feat_sel == 1) ? 0x1u : 0x0u; break; /* VERSION_1=bit32 */
    case R_QUEUE_NUM_MAX: r = (vrng.queue_sel == 0) ? QUEUE_NUM_MAX : 0; break;
    case R_QUEUE_READY:   r = vrng.queue_ready; break;
    case R_INT_STATUS:    r = vrng.int_status; break;
    case R_STATUS:        r = vrng.status; break;
    case R_CONFIG_GEN:    r = 0; break;
    default:              r = 0; break;
    }
    *val = r;
    return;
  }

  /* Write. */
  uint32_t w = (uint32_t)*val;
  switch (off) {
  case R_DEVICE_FEAT_SEL: vrng.dev_feat_sel = w; break;
  case R_DRIVER_FEAT_SEL: vrng.drv_feat_sel = w; break;
  case R_DRIVER_FEAT:     /* accept; we only offer VERSION_1, no validation */ break;
  case R_QUEUE_SEL:       vrng.queue_sel = w; break;
  case R_QUEUE_NUM:       vrng.queue_num = w; break;
  case R_QUEUE_READY:
    vrng.queue_ready = w & 1;
    if (!vrng.queue_ready) { vrng.last_avail_idx = 0; vrng.used_idx = 0; }
    break;
  case R_QUEUE_DESC_LO:   vrng.desc_ipa = (vrng.desc_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DESC_HI:   vrng.desc_ipa = (vrng.desc_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DRIVER_LO: vrng.driver_ipa = (vrng.driver_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DRIVER_HI: vrng.driver_ipa = (vrng.driver_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_DEVICE_LO: vrng.device_ipa = (vrng.device_ipa & 0xFFFFFFFF00000000ULL) | w; break;
  case R_QUEUE_DEVICE_HI: vrng.device_ipa = (vrng.device_ipa & 0xFFFFFFFFULL) | ((uint64_t)w << 32); break;
  case R_QUEUE_NOTIFY:    if (w == 0) virtio_process_queue(); break; /* queue 0 */
  case R_INT_ACK:         vrng.int_status &= ~w; break;
  case R_STATUS:
    vrng.status = w;
    if (w == 0) { /* reset */
      vrng.queue_ready = 0; vrng.queue_num = 0; vrng.int_status = 0;
      vrng.last_avail_idx = 0; vrng.used_idx = 0;
      vrng.desc_ipa = vrng.driver_ipa = vrng.device_ipa = 0;
    }
    break;
  default: break;
  }
}
