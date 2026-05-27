#include "net.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "strings/strings.h"

/* Page-aligned backing memory for both virtqueues. RX is queue 0, TX is
 * queue 1. Each queue gets its own descriptor table, available ring, and
 * used ring. */
static struct virtq_desc rx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail rx_avail __attribute__((aligned(4096)));
static struct virtq_used  rx_used  __attribute__((aligned(4096)));

static struct virtq_desc tx_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail tx_avail __attribute__((aligned(4096)));
static struct virtq_used  tx_used  __attribute__((aligned(4096)));

static struct virtio_net net_dev;

/* TX header (modern, 12 bytes). All zero for plain Ethernet — no GSO,
 * no checksum offload. We keep one global instance because net_tx is
 * synchronous (poll-completed) so re-use is safe. */
static struct virtio_net_hdr tx_hdr __attribute__((aligned(16)));

int net_tx(const void *frame, uint32_t len) {
  if (!frame || len == 0) {
    return -1;
  }

  memset(&tx_hdr, 0, sizeof(tx_hdr));

  struct virtq_seg segs[2] = {
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&tx_hdr), VIRTIO_NET_HDR_LEN,
       VIRTQ_DESC_F_NONE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)frame), len, VIRTQ_DESC_F_NONE},
  };

  virtqueue_submit_chain(&net_dev.tx_vq, segs, 2);
  virtqueue_notify(&net_dev.tx_vq);
  virtqueue_poll(&net_dev.tx_vq);

  return (int)len;
}

/* Build a 60-byte (Ethernet minimum) ARP request asking who has 10.0.2.2
 * (QEMU's slirp gateway), broadcast over the LAN. We embed our MAC as the
 * sender hardware address and 10.0.2.15 as the sender protocol address
 * (the slirp default for the first guest). */
static uint8_t arp_frame[60] __attribute__((aligned(16)));

int net_send_arp_probe(void) {
  if (!net_dev.have_mac) {
    uart_errorln("[NET] arp_probe: no MAC negotiated");
    return -1;
  }

  memset(arp_frame, 0, sizeof(arp_frame));

  /* Ethernet header (14 bytes) */
  for (int i = 0; i < 6; i++) {
    arp_frame[i] = 0xFF; /* dst = broadcast */
  }
  for (int i = 0; i < 6; i++) {
    arp_frame[6 + i] = net_dev.mac[i]; /* src */
  }
  arp_frame[12] = 0x08;
  arp_frame[13] = 0x06; /* ethertype = ARP */

  /* ARP body (28 bytes) */
  uint8_t *a = &arp_frame[14];
  a[0] = 0x00; a[1] = 0x01;       /* HTYPE = Ethernet */
  a[2] = 0x08; a[3] = 0x00;       /* PTYPE = IPv4 */
  a[4] = 6;                        /* HLEN */
  a[5] = 4;                        /* PLEN */
  a[6] = 0x00; a[7] = 0x01;       /* OPER = request */
  for (int i = 0; i < 6; i++) {
    a[8 + i] = net_dev.mac[i];     /* SHA (sender HW) */
  }
  /* SPA (sender IP) = 10.0.2.15 */
  a[14] = 10; a[15] = 0; a[16] = 2; a[17] = 15;
  /* THA (target HW) = 0; already zeroed */
  /* TPA (target IP) = 10.0.2.2 (slirp gateway) */
  a[24] = 10; a[25] = 0; a[26] = 2; a[27] = 2;

  uart_println("[NET] Sending ARP probe for 10.0.2.2 (slirp gateway)");
  return net_tx(arp_frame, sizeof(arp_frame));
}


/* RX path. We pre-fill the RX queue with a small bank of fixed-size
 * buffers; the device fills them as packets arrive. net_rx_poll drains
 * one used entry, copies the payload to the caller (skipping the
 * virtio_net_hdr), and re-arms the same buffer.
 *
 * Buffer ↔ descriptor mapping: virtqueue_submit allocates from free_head
 * and doesn't return the index it picked, so we shadow it with our own
 * counter and update rx_desc_to_buf[] in lockstep with each submit. */
#define NET_RX_BUF_COUNT 8
#define NET_RX_BUF_SIZE  1600 /* 12B hdr + 1500 MTU + slack */

static uint8_t rx_bufs[NET_RX_BUF_COUNT][NET_RX_BUF_SIZE]
    __attribute__((aligned(64)));
static int     rx_desc_to_buf[VIRTQ_MAX_SIZE];
static uint8_t rx_initialized;

static void net_rx_submit_buf(int buf_idx) {
  uint16_t desc_id = net_dev.rx_vq.free_head;
  uint64_t pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)rx_bufs[buf_idx]);
  virtqueue_submit(&net_dev.rx_vq, pa, NET_RX_BUF_SIZE, VIRTQ_DESC_F_WRITE);
  rx_desc_to_buf[desc_id] = buf_idx;
}

static void net_rx_init(void) {
  for (int i = 0; i < (int)VIRTQ_MAX_SIZE; i++) {
    rx_desc_to_buf[i] = -1;
  }
  for (int i = 0; i < NET_RX_BUF_COUNT; i++) {
    net_rx_submit_buf(i);
  }
  virtqueue_notify(&net_dev.rx_vq);
  rx_initialized = 1;
  uart_printf("[NET] RX queue primed with %d buffers (%d bytes each)\n",
              (uint64_t)NET_RX_BUF_COUNT, (uint64_t)NET_RX_BUF_SIZE);
}

int net_rx_poll(void *dst, uint32_t max_len) {
  if (!rx_initialized) {
    return -1;
  }

  uint16_t used_now = *(volatile uint16_t *)&net_dev.rx_vq.used->idx;
  if (net_dev.rx_vq.last_used == used_now) {
    return 0; /* nothing pending */
  }
  dsb_sy();

  uint16_t slot      = net_dev.rx_vq.last_used % net_dev.rx_vq.size;
  uint32_t desc_id   = net_dev.rx_vq.used->ring[slot].id;
  uint32_t total_len = net_dev.rx_vq.used->ring[slot].len;
  net_dev.rx_vq.last_used++;

  if (desc_id >= VIRTQ_MAX_SIZE || rx_desc_to_buf[desc_id] < 0) {
    uart_errorln("[NET] rx_poll: bogus / unmapped descriptor");
    return -1;
  }
  int buf_idx = rx_desc_to_buf[desc_id];
  rx_desc_to_buf[desc_id] = -1;

  uint8_t *buf = rx_bufs[buf_idx];
  int copied = 0;
  if (total_len >= VIRTIO_NET_HDR_LEN) {
    uint32_t frame_len = total_len - VIRTIO_NET_HDR_LEN;
    uint32_t to_copy   = (frame_len < max_len) ? frame_len : max_len;
    if (dst && to_copy > 0) {
      memcpy(dst, buf + VIRTIO_NET_HDR_LEN, to_copy);
    }
    copied = (int)to_copy;
  }

  net_rx_submit_buf(buf_idx);
  virtqueue_notify(&net_dev.rx_vq);
  return copied;
}


/* ----- L3 helpers: ARP-reply parser, IPv4/ICMP checksum, ICMP echo. ----- */

static uint8_t gateway_mac[6];
static uint8_t have_gateway_mac;

/* RFC 1071 "internet checksum": 16-bit one's complement sum of all 16-bit
 * words, then complement. Reads bytes in network order so we don't depend
 * on host endianness. The returned value is the network-order checksum;
 * the caller stores it as MSB then LSB. */
static uint16_t inet_csum(const uint8_t *data, uint32_t len) {
  uint32_t sum = 0;
  while (len >= 2) {
    sum += ((uint32_t)data[0] << 8) | data[1];
    data += 2;
    len -= 2;
  }
  if (len == 1) {
    sum += (uint32_t)data[0] << 8;
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return (uint16_t)~sum;
}

/* If `frame` is an ARP reply, store the sender hardware address as our
 * cached gateway MAC. Silently ignored otherwise. */
static void parse_arp_reply(const uint8_t *frame, uint32_t len) {
  if (len < 42)                       return; /* eth14 + arp28 */
  if (frame[12] != 0x08 || frame[13] != 0x06) return; /* ethertype != ARP */
  const uint8_t *a = &frame[14];
  if (a[6] != 0x00 || a[7] != 0x02)   return; /* OPER != reply */

  for (int i = 0; i < 6; i++) {
    gateway_mac[i] = a[8 + i];
  }
  have_gateway_mac = 1;
  uart_printf("[NET] Learned gateway MAC: %x:%x:%x:%x:%x:%x\n",
              (uint64_t)gateway_mac[0], (uint64_t)gateway_mac[1],
              (uint64_t)gateway_mac[2], (uint64_t)gateway_mac[3],
              (uint64_t)gateway_mac[4], (uint64_t)gateway_mac[5]);
}

#define ICMP_PING_PAYLOAD 56
static uint8_t ping_frame[14 + 20 + 8 + ICMP_PING_PAYLOAD]
    __attribute__((aligned(16)));

/* Send an ICMP echo request to 10.0.2.2 (slirp gateway). Requires the
 * gateway MAC to be learned (via an ARP exchange). Returns bytes sent or
 * negative on error. */
static int net_send_ping(uint16_t seq) {
  if (!have_gateway_mac) {
    uart_errorln("[NET] ping: no gateway MAC (run ARP first)");
    return -1;
  }

  memset(ping_frame, 0, sizeof(ping_frame));

  /* Ethernet header */
  for (int i = 0; i < 6; i++) ping_frame[i]     = gateway_mac[i];
  for (int i = 0; i < 6; i++) ping_frame[6 + i] = net_dev.mac[i];
  ping_frame[12] = 0x08;
  ping_frame[13] = 0x00; /* ethertype = IPv4 */

  /* IPv4 header (20 bytes) */
  uint8_t *ip = &ping_frame[14];
  ip[0] = 0x45;                     /* version=4, IHL=5 */
  ip[1] = 0;                        /* TOS */
  uint16_t total = 20 + 8 + ICMP_PING_PAYLOAD;
  ip[2] = (total >> 8) & 0xFF;
  ip[3] =  total       & 0xFF;
  ip[4] = 0; ip[5] = 0;             /* id */
  ip[6] = 0; ip[7] = 0;             /* flags + frag offset */
  ip[8] = 64;                       /* TTL */
  ip[9] = 1;                        /* proto = ICMP */
  ip[10] = 0; ip[11] = 0;           /* csum (placeholder) */
  /* src = 10.0.2.15, dst = 10.0.2.2 */
  ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 15;
  ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 2;

  uint16_t ipcsum = inet_csum(ip, 20);
  ip[10] = (ipcsum >> 8) & 0xFF;
  ip[11] =  ipcsum       & 0xFF;

  /* ICMP echo request, 8-byte header + payload */
  uint8_t *icmp = &ping_frame[14 + 20];
  icmp[0] = 8;                      /* type = echo request */
  icmp[1] = 0;                      /* code */
  icmp[2] = 0; icmp[3] = 0;         /* csum (placeholder) */
  icmp[4] = 0; icmp[5] = 42;        /* identifier = 42 */
  icmp[6] = (seq >> 8) & 0xFF;
  icmp[7] =  seq       & 0xFF;
  for (int i = 0; i < ICMP_PING_PAYLOAD; i++) {
    icmp[8 + i] = (uint8_t)('a' + (i % 26));
  }

  uint16_t iccsum = inet_csum(icmp, 8 + ICMP_PING_PAYLOAD);
  icmp[2] = (iccsum >> 8) & 0xFF;
  icmp[3] =  iccsum       & 0xFF;

  uart_printf("[NET] Sending ICMP echo request seq=%d to 10.0.2.2\n",
              (uint64_t)seq);
  return net_tx(ping_frame, sizeof(ping_frame));
}


void pci_virtio_net_init(void) {
  uart_println("[NET] Initializing Device");

  /* Step 0: Find device on the PCI bus */
  if (pci_find_device(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID,
                      &net_dev.pci) != ESUCCESS) {
    uart_errorln("[NET] Device not found");
    return;
  }
  uart_println("[NET] Device found");

  if ((pci_get_header_type(&net_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[NET] Unexpected header type");
    return;
  }

  pci_assign_bars(&net_dev.pci);
  pci_enable_device(&net_dev.pci);
  virtio_parse_capabilities(&net_dev.pci, &net_dev.pci_caps);

  /* VirtIO Device Init Sequence (§3.1.1). All register accesses through MMIO. */
  uintptr_t base = net_dev.pci_caps.common_cfg;

  /* Step 1: Reset Device */
  uart_println("[NET][VIRTIO-INIT][1] Reset Device");
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }
  uart_println("[NET][VIRTIO-INIT][1] Reset Device Complete");

  /* Step 2: ACK */
  uart_println("[NET][VIRTIO-INIT][2] Ack");
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS,
              status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: Set Driver status */
  uart_println("[NET][VIRTIO-INIT][3] Driver Status");
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: Feature Negotiation */
  uart_println("[NET][VIRTIO-INIT][4] Negotiate Features");

  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[0]: %x\n", feat_lo);

  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);
  uart_printf(" Device features[1]: %x\n", feat_hi);

  /* VIRTIO_F_VERSION_1 (bit 32 → feat_hi bit 0) is required for the modern
   * device path. Refuse to drive a device that doesn't offer it. */
  if (!(feat_hi & 0x01)) {
    uart_errorln("[NET] Device does not advertise VIRTIO_F_VERSION_1");
    return;
  }

  /* Accept MAC and STATUS if offered, plus VERSION_1. Reject every GSO /
   * checksum-offload feature for now — TX/RX paths assume plain frames. */
  uint32_t want_lo = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
  uint32_t guest_lo = feat_lo & want_lo;
  uint32_t guest_hi = feat_hi & 0x01;

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
  dsb_sy();

  uart_printf(" Accepted Features: lo=%x hi=%x\n", guest_lo, guest_hi);

  /* Step 5: FEATURES_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS,
              status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();

  /* Step 6a: Re-read and verify FEATURES_OK stuck */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[NET] FEATURES_OK failed");
    return;
  }
  uart_printf("[NET] Status: %x\n", (uint32_t)status);
  uart_println("[NET] FEATURES_OK !");

  /* Step 6b: Setup virtqueues. RX (0) and TX (1). */
  net_dev.rx_vq.desc  = rx_desc;
  net_dev.rx_vq.avail = &rx_avail;
  net_dev.rx_vq.used  = &rx_used;
  if (virtqueue_setup(base, VIRTIO_NET_QUEUE_RX, &net_dev.rx_vq,
                      &net_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[NET] RX virtqueue setup failed");
    return;
  }

  net_dev.tx_vq.desc  = tx_desc;
  net_dev.tx_vq.avail = &tx_avail;
  net_dev.tx_vq.used  = &tx_used;
  if (virtqueue_setup(base, VIRTIO_NET_QUEUE_TX, &net_dev.tx_vq,
                      &net_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[NET] TX virtqueue setup failed");
    return;
  }

  /* Step 7: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();
  uart_println("[NET] DRIVER_OK set");

  /* Pre-fill the RX queue immediately after DRIVER_OK so a slirp ARP
   * reply has buffers waiting for it before we send the request. */
  net_rx_init();

  /* Read MAC and link status from device cfg. With F_MAC negotiated the
   * device has placed a 6-byte MAC at offset 0; without it we'd have to
   * generate one ourselves. */
  uintptr_t dcfg = net_dev.pci_caps.device_cfg;

  if (guest_lo & VIRTIO_NET_F_MAC) {
    for (int i = 0; i < 6; i++) {
      net_dev.mac[i] = mmio_read8(dcfg + VIRTIO_NET_CFG_MAC + i);
    }
    net_dev.have_mac = 1;
    uart_printf("[NET] MAC: %x:%x:%x:%x:%x:%x\n",
                (uint64_t)net_dev.mac[0], (uint64_t)net_dev.mac[1],
                (uint64_t)net_dev.mac[2], (uint64_t)net_dev.mac[3],
                (uint64_t)net_dev.mac[4], (uint64_t)net_dev.mac[5]);
  } else {
    uart_println("[NET] Device did not advertise VIRTIO_NET_F_MAC");
  }

  if (guest_lo & VIRTIO_NET_F_STATUS) {
    net_dev.link_status = mmio_read16(dcfg + VIRTIO_NET_CFG_STATUS);
    net_dev.have_status = 1;
    uart_printf("[NET] Link: %s (status=%x)\n",
                (net_dev.link_status & VIRTIO_NET_S_LINK_UP) ? "UP" : "DOWN",
                (uint64_t)net_dev.link_status);
  }


  /* Smoke-test the wire by sending a broadcast ARP request to the slirp
   * gateway, then poll the RX queue briefly to catch the reply. The reply
   * is generated synchronously by QEMU, so it should appear within a few
   * thousand spins; we cap the wait so a wedged device cannot hang boot. */
  if (net_send_arp_probe() > 0) {
    uart_println("[NET] ARP probe TX accepted by device");
  }

  /* RX bookkeeping for both the ARP reply and the ICMP echo reply. */
  uint8_t rx_buf[256];

  for (uint32_t spins = 0; spins < 1000000u; spins++) {
    int n = net_rx_poll(rx_buf, sizeof(rx_buf));
    if (n > 0) {
      uart_printf("[NET] RX: %d bytes", (uint64_t)n);
      if (n >= 14) {
        uint64_t ethertype = ((uint64_t)rx_buf[12] << 8) | rx_buf[13];
        uart_printf(" type=%x src=%x:%x:%x:%x:%x:%x",
                    ethertype,
                    (uint64_t)rx_buf[6],  (uint64_t)rx_buf[7],
                    (uint64_t)rx_buf[8],  (uint64_t)rx_buf[9],
                    (uint64_t)rx_buf[10], (uint64_t)rx_buf[11]);
      }
      uart_println("");
      parse_arp_reply(rx_buf, (uint32_t)n);
      break;
    }
  }

  /* Full L3 round-trip: now that we know slirp's MAC, fire an ICMP echo
   * request and watch for the matching reply. */
  if (have_gateway_mac && net_send_ping(1) > 0) {
    for (uint32_t spins = 0; spins < 2000000u; spins++) {
      int n = net_rx_poll(rx_buf, sizeof(rx_buf));
      if (n >= 14 + 20 + 8 &&
          rx_buf[12] == 0x08 && rx_buf[13] == 0x00 /* IPv4 */) {
        const uint8_t *ip   = &rx_buf[14];
        const uint8_t *icmp = &rx_buf[14 + 20];
        if (ip[9] == 1 /* ICMP */ && icmp[0] == 0 /* echo reply */) {
          uart_printf(
              "[NET] PING reply from %d.%d.%d.%d ttl=%d seq=%d \\o/\n",
              (uint64_t)ip[12], (uint64_t)ip[13],
              (uint64_t)ip[14], (uint64_t)ip[15],
              (uint64_t)ip[8],
              (uint64_t)(((uint32_t)icmp[6] << 8) | icmp[7]));
          break;
        }
      }
    }
  }

}
