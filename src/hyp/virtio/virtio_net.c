#include "virtio_net.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include "hyp_alloc.h"
#include <stdint.h>

/* virtio-mmio register offsets (same modern layout as rng/blk). */
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
#define R_CONFIG         0x100 /* net config: 6-byte MAC at offset 0 */

#define MAGIC_VALUE 0x74726976UL
#define DEVICE_ID_NET 1
#define VENDOR_ID 0x494D5246UL
#define QUEUE_NUM_MAX 64
#define NQUEUES 2

#define ST_DRIVER_OK 4
#define INT_VRING 0x1
#define DESC_F_NEXT 0x1
#define DESC_F_WRITE 0x2

#define NET_BUF_MAX 1600 /* clamp a copied frame (hdr + MTU-ish) */

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed)) virtq_desc_t;

/* Per-queue state. */
typedef struct {
  uint32_t num, ready;
  uint64_t desc_ipa, driver_ipa, device_ipa;
  uint16_t last_avail_idx, used_idx;
} netq_t;

static struct {
  uint32_t status, dev_feat_sel, int_status;
  uint32_t queue_sel;
  netq_t   q[NQUEUES];
} vnet;

/* Fixed device MAC: 52:46:52:4D:49:01 ("RF RMI" ish). */
static const uint8_t net_mac[6] = { 0x52, 0x46, 0x52, 0x4D, 0x49, 0x01 };

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

/* Read the head descriptor of the next available entry on queue `qi` WITHOUT
 * consuming it. Returns the head index, the buffer IPA/len, or -1 if none. */
static int net_peek_avail(netq_t *q, uint16_t *out_head, uint64_t *out_addr,
                          uint32_t *out_len, int *out_is_write) {
  uint16_t avail_idx;
  if (!gread(q->driver_ipa + 2, &avail_idx, 2)) return -1;
  if ((uint16_t)(avail_idx - q->last_avail_idx) == 0) return -1;
  uint16_t slot = (uint16_t)(q->last_avail_idx % q->num);
  uint16_t head;
  if (!gread(q->driver_ipa + 4 + 2 * slot, &head, 2)) return -1;
  if (head >= q->num) return -1;
  virtq_desc_t d;
  if (!gread(q->desc_ipa + 16ULL * head, &d, sizeof(d))) return -1;
  *out_head = head;
  *out_addr = d.addr;
  *out_len = d.len;
  *out_is_write = (d.flags & DESC_F_WRITE) != 0;
  return 0;
}

/* Complete a buffer on queue `qi`: post used elem {head,len}, advance used.idx. */
static void net_complete(netq_t *q, uint16_t head, uint32_t len) {
  uint32_t elem[2] = { head, len };
  gwrite(q->device_ipa + 4 + 8ULL * (q->used_idx % q->num), elem, sizeof(elem));
  q->last_avail_idx++;
  __asm__ __volatile__("dsb ish" ::: "memory");
  q->used_idx++;
  gwrite(q->device_ipa + 2, &q->used_idx, 2);
  __asm__ __volatile__("dsb ish" ::: "memory");
}

/* TX notify: loopback each transmitted frame into a posted RX buffer. */
static void virtio_net_process_tx(void) {
  netq_t *tx = &vnet.q[VIRTIO_NET_TXQ];
  netq_t *rx = &vnet.q[VIRTIO_NET_RXQ];
  if (!(vnet.status & ST_DRIVER_OK) || !tx->ready || !rx->ready) return;
  if (tx->num == 0 || rx->num == 0) return;

  int rx_delivered = 0;
  static uint8_t frame[NET_BUF_MAX];

  for (uint32_t guard = 0; guard < tx->num; guard++) {
    uint16_t txh; uint64_t txa; uint32_t txl; int txw;
    if (net_peek_avail(tx, &txh, &txa, &txl, &txw) != 0) break; /* no more TX */

    /* Copy the transmitted frame (virtio-net hdr + payload) out of the guest. */
    uint32_t n = txl; if (n > NET_BUF_MAX) n = NET_BUF_MAX;
    if (!gread(txa, frame, n)) { net_complete(tx, txh, 0); continue; }

    /* Grab a posted RX buffer to loop it back into. */
    uint16_t rxh; uint64_t rxa; uint32_t rxl; int rxw;
    if (net_peek_avail(rx, &rxh, &rxa, &rxl, &rxw) == 0) {
      uint32_t m = (n <= rxl) ? n : rxl;
      gwrite(rxa, frame, m);     /* deliver looped frame -> guest RX buffer */
      net_complete(rx, rxh, m);  /* RX buffer used, len=m */
      rx_delivered = 1;
    }
    net_complete(tx, txh, 0);    /* TX buffer reclaimed */
  }

  if (rx_delivered) {
    vnet.int_status |= INT_VRING;
    vgic_inject_ppi(VIRTIO_NET_SPI);
  }
}

/* Select the per-queue state for the current QueueSel (only 0/1 valid). */
static netq_t *cur_q(void) {
  return (vnet.queue_sel < NQUEUES) ? &vnet.q[vnet.queue_sel] : &vnet.q[0];
}

int virtio_net_mmio_is_target(uint64_t ipa) {
  return ipa >= VIRTIO_NET_MMIO_BASE &&
         ipa < VIRTIO_NET_MMIO_BASE + VIRTIO_NET_MMIO_SIZE;
}

void virtio_net_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                             int size_bytes) {
  (void)size_bytes;
  uint64_t off = ipa - VIRTIO_NET_MMIO_BASE;

  if (!is_write) {
    uint32_t r = 0;
    switch (off) {
    case R_MAGIC:         r = MAGIC_VALUE; break;
    case R_VERSION:       r = 2; break;
    case R_DEVICE_ID:     r = DEVICE_ID_NET; break;
    case R_VENDOR_ID:     r = VENDOR_ID; break;
    case R_DEVICE_FEAT:   r = (vnet.dev_feat_sel == 1) ? 0x1u : 0x0u; break;
    case R_QUEUE_NUM_MAX: r = (vnet.queue_sel < NQUEUES) ? QUEUE_NUM_MAX : 0; break;
    case R_QUEUE_READY:   r = cur_q()->ready; break;
    case R_INT_STATUS:    r = vnet.int_status; break;
    case R_STATUS:        r = vnet.status; break;
    case R_CONFIG_GEN:    r = 0; break;
    /* MAC bytes [0..5] little-endian across the two config words. */
    case R_CONFIG + 0:
      r = net_mac[0] | (net_mac[1] << 8) | (net_mac[2] << 16) | (net_mac[3] << 24);
      break;
    case R_CONFIG + 4:
      r = net_mac[4] | (net_mac[5] << 8);
      break;
    default:              r = 0; break;
    }
    *val = r;
    return;
  }

  uint32_t w = (uint32_t)*val;
  netq_t *q = cur_q();
  switch (off) {
  case R_DEVICE_FEAT_SEL: vnet.dev_feat_sel = w; break;
  case R_DRIVER_FEAT_SEL: break;
  case R_DRIVER_FEAT:     break;
  case R_QUEUE_SEL:       vnet.queue_sel = w; break;
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
    /* Only the TX queue kick does work (loopback); RX kicks just post buffers. */
    if (w == VIRTIO_NET_TXQ) virtio_net_process_tx();
    break;
  case R_INT_ACK:         vnet.int_status &= ~w; break;
  case R_STATUS:
    vnet.status = w;
    if (w == 0) {
      vnet.int_status = 0;
      for (int i = 0; i < NQUEUES; i++) {
        vnet.q[i].ready = vnet.q[i].num = 0;
        vnet.q[i].last_avail_idx = vnet.q[i].used_idx = 0;
        vnet.q[i].desc_ipa = vnet.q[i].driver_ipa = vnet.q[i].device_ipa = 0;
      }
    }
    break;
  default: break;
  }
}
