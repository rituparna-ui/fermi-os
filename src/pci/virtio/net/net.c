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

/* Per-direction packet counters — surfaced via /proc/netinfo. */
static uint64_t rx_packets;
static uint64_t tx_packets;

/* IPv4 + DHCP state. Initialised to slirp defaults so the early ARP/ICMP
 * smoke tests work even before DHCP runs; dhcp_acquire() then overwrites
 * them with the real lease. */
uint8_t  g_my_ip[4]       = {10, 0, 2, 15};
uint8_t  g_subnet_mask[4] = {255, 255, 255, 0};
uint8_t  g_gateway_ip[4]  = {10, 0, 2, 2};
uint8_t  g_dhcp_server[4] = {0, 0, 0, 0};
uint32_t g_lease_secs     = 0;
uint8_t  g_dhcp_acquired  = 0;

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

  tx_packets++;

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
  /* SPA (sender IP) — current lease (slirp default until DHCP runs) */
  for (int i = 0; i < 4; i++) a[14 + i] = g_my_ip[i];
  /* THA (target HW) = 0; already zeroed */
  /* TPA (target IP) — gateway */
  for (int i = 0; i < 4; i++) a[24 + i] = g_gateway_ip[i];

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
  if (copied > 0) {
    rx_packets++;
  }

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
int net_send_ping(uint16_t seq) {
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
  /* src = our leased IP, dst = gateway */
  for (int i = 0; i < 4; i++) ip[12 + i] = g_my_ip[i];
  for (int i = 0; i < 4; i++) ip[16 + i] = g_gateway_ip[i];

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


/* Render a /proc-style snapshot of the device state. Caller-allocated
 * buffer; safe to truncate. Returns bytes written. */
int net_get_info(char *buf, uint32_t buflen) {
  static const char hex[] = "0123456789abcdef";
  uint32_t pos = 0;

  /* MAC */
  static const char mac_label[] = "mac:        ";
  for (uint32_t i = 0; i < sizeof(mac_label) - 1 && pos < buflen; i++) {
    buf[pos++] = mac_label[i];
  }
  for (int i = 0; i < 6 && pos + 3 <= buflen; i++) {
    if (i > 0) buf[pos++] = ':';
    buf[pos++] = hex[net_dev.mac[i] >> 4];
    buf[pos++] = hex[net_dev.mac[i] & 0xF];
  }
  if (pos < buflen) buf[pos++] = '\n';

  /* Link */
  int n = ksnprintf(buf + pos, buflen - pos, "link:       %s\n",
                    (net_dev.link_status & VIRTIO_NET_S_LINK_UP) ? "UP"
                                                                 : "DOWN");
  if (n > 0) pos += (uint32_t)n;

  /* IPv4 config (DHCP-acquired or default slirp values) */
  n = ksnprintf(buf + pos, buflen - pos,
                "ip:         %d.%d.%d.%d\n"
                "netmask:    %d.%d.%d.%d\n"
                "gateway:    %d.%d.%d.%d\n"
                "dhcp:       %s\n"
                "dhcp_srv:   %d.%d.%d.%d\n"
                "lease:      %u s\n",
                g_my_ip[0], g_my_ip[1], g_my_ip[2], g_my_ip[3],
                g_subnet_mask[0], g_subnet_mask[1],
                g_subnet_mask[2], g_subnet_mask[3],
                g_gateway_ip[0], g_gateway_ip[1],
                g_gateway_ip[2], g_gateway_ip[3],
                g_dhcp_acquired ? "yes" : "no",
                g_dhcp_server[0], g_dhcp_server[1],
                g_dhcp_server[2], g_dhcp_server[3],
                g_lease_secs);
  if (n > 0) pos += (uint32_t)n;

  /* Gateway MAC (learned via ARP) */
  static const char gw_label[] = "gw_mac:     ";
  for (uint32_t i = 0; i < sizeof(gw_label) - 1 && pos < buflen; i++) {
    buf[pos++] = gw_label[i];
  }
  if (have_gateway_mac) {
    for (int i = 0; i < 6 && pos + 3 <= buflen; i++) {
      if (i > 0) buf[pos++] = ':';
      buf[pos++] = hex[gateway_mac[i] >> 4];
      buf[pos++] = hex[gateway_mac[i] & 0xF];
    }
  } else {
    static const char unknown[] = "(unknown)";
    for (uint32_t i = 0; i < sizeof(unknown) - 1 && pos < buflen; i++) {
      buf[pos++] = unknown[i];
    }
  }
  if (pos < buflen) buf[pos++] = '\n';

  /* Counters */
  n = ksnprintf(buf + pos, buflen - pos,
                "rx_packets: %u\n"
                "tx_packets: %u\n",
                rx_packets, tx_packets);
  if (n > 0) pos += (uint32_t)n;

  return (int)pos;
}


/* ============================ DHCP CLIENT (RFC 2131) ===========================
 *
 * Minimal four-step exchange against slirp's built-in DHCP server:
 *
 *   guest -> 255.255.255.255:67  DHCPDISCOVER (option 53 = 1)
 *   guest <- 10.0.2.2:68         DHCPOFFER    (yiaddr filled in)
 *   guest -> 255.255.255.255:67  DHCPREQUEST  (option 50 = offered IP,
 *                                              option 54 = server id)
 *   guest <- 10.0.2.2:68         DHCPACK      (commits the lease)
 *
 * UDP checksum is left at 0 (legal for IPv4). All buffers live in BSS.
 * Synchronous: each TX polls until acked, then we busy-poll RX for the
 * matching reply, capped so a wedged server can't hang boot.
 */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_MAGIC_0 0x63
#define DHCP_MAGIC_1 0x82
#define DHCP_MAGIC_2 0x53
#define DHCP_MAGIC_3 0x63

#define DHCP_OPT_SUBNET    1
#define DHCP_OPT_ROUTER    3
#define DHCP_OPT_REQ_IP    50
#define DHCP_OPT_LEASE     51
#define DHCP_OPT_MSGTYPE   53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_PARAM_REQ 55
#define DHCP_OPT_END       255

/* DHCP frame buffer: Eth(14) + IP(20) + UDP(8) + BOOTP(236) + magic(4) +
 * options(<=64). Aligned for the virtqueue. */
static uint8_t dhcp_frame[14 + 20 + 8 + 240 + 64] __attribute__((aligned(16)));
static uint8_t bootp_buf[240 + 64]                __attribute__((aligned(16)));

/* Build an Ethernet/IPv4/UDP frame around `payload`. Writes into `frame`,
 * returns the total frame length on the wire. */
static uint32_t udp_build(uint8_t *frame,
                          const uint8_t dst_mac[6],
                          const uint8_t src_ip[4],
                          const uint8_t dst_ip[4],
                          uint16_t src_port, uint16_t dst_port,
                          const uint8_t *payload, uint32_t payload_len) {
  uint32_t udp_len = 8 + payload_len;
  uint32_t ip_len  = 20 + udp_len;
  uint32_t total   = 14 + ip_len;

  /* Ethernet */
  for (int i = 0; i < 6; i++) frame[i]     = dst_mac[i];
  for (int i = 0; i < 6; i++) frame[6 + i] = net_dev.mac[i];
  frame[12] = 0x08; frame[13] = 0x00;

  /* IPv4 */
  uint8_t *ip = &frame[14];
  ip[0] = 0x45;
  ip[1] = 0;
  ip[2] = (ip_len >> 8) & 0xFF;
  ip[3] =  ip_len       & 0xFF;
  ip[4] = 0; ip[5] = 0;
  ip[6] = 0; ip[7] = 0;
  ip[8] = 64;
  ip[9] = 17; /* UDP */
  ip[10] = 0; ip[11] = 0;
  for (int i = 0; i < 4; i++) ip[12 + i] = src_ip[i];
  for (int i = 0; i < 4; i++) ip[16 + i] = dst_ip[i];
  uint16_t ipcsum = inet_csum(ip, 20);
  ip[10] = (ipcsum >> 8) & 0xFF;
  ip[11] =  ipcsum       & 0xFF;

  /* UDP — checksum 0 (legal for IPv4). */
  uint8_t *udp = &frame[14 + 20];
  udp[0] = (src_port >> 8) & 0xFF;
  udp[1] =  src_port       & 0xFF;
  udp[2] = (dst_port >> 8) & 0xFF;
  udp[3] =  dst_port       & 0xFF;
  udp[4] = (udp_len >> 8) & 0xFF;
  udp[5] =  udp_len       & 0xFF;
  udp[6] = 0; udp[7] = 0;

  /* Payload */
  for (uint32_t i = 0; i < payload_len; i++) udp[8 + i] = payload[i];

  return total;
}

/* Build a DHCP DISCOVER or REQUEST in `bootp`. Returns total bytes written
 * (240 fixed header + magic + variable options). */
static uint32_t dhcp_build(uint8_t *bootp, uint8_t msg_type,
                           const uint8_t client_mac[6], uint32_t xid,
                           const uint8_t *requested_ip,
                           const uint8_t *server_id) {
  memset(bootp, 0, 240);
  bootp[0] = 1;  /* BOOTREQUEST */
  bootp[1] = 1;  /* htype = Ethernet */
  bootp[2] = 6;  /* hlen */
  bootp[3] = 0;  /* hops */
  bootp[4] = (xid >> 24) & 0xFF;
  bootp[5] = (xid >> 16) & 0xFF;
  bootp[6] = (xid >>  8) & 0xFF;
  bootp[7] =  xid        & 0xFF;
  /* secs, flags, ciaddr, yiaddr, siaddr, giaddr all zero */
  for (int i = 0; i < 6; i++) bootp[28 + i] = client_mac[i];
  /* magic cookie */
  bootp[236] = DHCP_MAGIC_0;
  bootp[237] = DHCP_MAGIC_1;
  bootp[238] = DHCP_MAGIC_2;
  bootp[239] = DHCP_MAGIC_3;

  uint8_t *opt = bootp + 240;
  uint32_t p = 0;

  /* Option 53: message type */
  opt[p++] = DHCP_OPT_MSGTYPE;
  opt[p++] = 1;
  opt[p++] = msg_type;

  /* Option 55: parameter request list — what we want from the server */
  opt[p++] = DHCP_OPT_PARAM_REQ;
  opt[p++] = 4;
  opt[p++] = DHCP_OPT_SUBNET;
  opt[p++] = DHCP_OPT_ROUTER;
  opt[p++] = DHCP_OPT_LEASE;
  opt[p++] = DHCP_OPT_SERVER_ID;

  /* For REQUEST: include 50 (requested IP) and 54 (server identifier). */
  if (requested_ip) {
    opt[p++] = DHCP_OPT_REQ_IP;
    opt[p++] = 4;
    for (int i = 0; i < 4; i++) opt[p++] = requested_ip[i];
  }
  if (server_id) {
    opt[p++] = DHCP_OPT_SERVER_ID;
    opt[p++] = 4;
    for (int i = 0; i < 4; i++) opt[p++] = server_id[i];
  }

  opt[p++] = DHCP_OPT_END;
  return 240 + p;
}

/* Find option `want` in an options blob. Stores its length in *found_len.
 * Returns pointer to value bytes, or NULL if missing. */
static const uint8_t *dhcp_find_option(const uint8_t *opts, uint32_t max_len,
                                       uint8_t want, uint8_t *found_len) {
  uint32_t p = 0;
  while (p < max_len) {
    uint8_t code = opts[p];
    if (code == DHCP_OPT_END) return 0;
    if (code == 0) { p++; continue; }              /* PAD */
    if (p + 1 >= max_len) return 0;
    uint8_t len = opts[p + 1];
    if (p + 2 + len > max_len) return 0;
    if (code == want) {
      *found_len = len;
      return &opts[p + 2];
    }
    p += 2 + len;
  }
  return 0;
}

/* Validate that `frame` is a DHCP reply matching `expect_xid` and
 * `expect_msg`, then extract yiaddr and known options.
 * Returns 1 on match (out_* filled), 0 otherwise. */
static int dhcp_parse(const uint8_t *frame, uint32_t flen,
                      uint32_t expect_xid, uint8_t expect_msg,
                      uint8_t out_yiaddr[4], uint8_t out_server[4],
                      uint8_t out_mask[4],   uint8_t out_router[4],
                      uint32_t *out_lease) {
  if (flen < 14 + 20 + 8 + 240) return 0;
  if (frame[12] != 0x08 || frame[13] != 0x00) return 0;

  const uint8_t *ip = &frame[14];
  if ((ip[0] & 0xF0) != 0x40) return 0;
  if (ip[9] != 17) return 0; /* UDP */
  uint32_t ihl = (ip[0] & 0x0F) * 4;
  if (ihl != 20) return 0;

  const uint8_t *udp = ip + ihl;
  uint16_t src_port = ((uint16_t)udp[0] << 8) | udp[1];
  uint16_t dst_port = ((uint16_t)udp[2] << 8) | udp[3];
  uint16_t udp_len  = ((uint16_t)udp[4] << 8) | udp[5];
  if (src_port != 67 || dst_port != 68) return 0;
  if (udp_len < 8 + 240) return 0;

  const uint8_t *bootp = udp + 8;
  if (bootp[0] != 2) return 0; /* BOOTREPLY */
  uint32_t xid = ((uint32_t)bootp[4] << 24) | ((uint32_t)bootp[5] << 16) |
                 ((uint32_t)bootp[6] <<  8) |  bootp[7];
  if (xid != expect_xid) return 0;

  const uint8_t *magic = bootp + 236;
  if (magic[0] != DHCP_MAGIC_0 || magic[1] != DHCP_MAGIC_1 ||
      magic[2] != DHCP_MAGIC_2 || magic[3] != DHCP_MAGIC_3) return 0;

  uint32_t opts_len = (uint32_t)udp_len - 8 - 240;
  const uint8_t *opts = bootp + 240;

  uint8_t l;
  const uint8_t *mt = dhcp_find_option(opts, opts_len, DHCP_OPT_MSGTYPE, &l);
  if (!mt || l != 1 || mt[0] != expect_msg) return 0;

  for (int i = 0; i < 4; i++) out_yiaddr[i] = bootp[16 + i];

  const uint8_t *sid = dhcp_find_option(opts, opts_len, DHCP_OPT_SERVER_ID, &l);
  if (sid && l == 4) for (int i = 0; i < 4; i++) out_server[i] = sid[i];

  const uint8_t *sm = dhcp_find_option(opts, opts_len, DHCP_OPT_SUBNET, &l);
  if (sm && l == 4) for (int i = 0; i < 4; i++) out_mask[i] = sm[i];

  const uint8_t *rt = dhcp_find_option(opts, opts_len, DHCP_OPT_ROUTER, &l);
  if (rt && l >= 4) for (int i = 0; i < 4; i++) out_router[i] = rt[i];

  const uint8_t *ls = dhcp_find_option(opts, opts_len, DHCP_OPT_LEASE, &l);
  if (ls && l == 4) {
    *out_lease = ((uint32_t)ls[0] << 24) | ((uint32_t)ls[1] << 16) |
                 ((uint32_t)ls[2] <<  8) |  ls[3];
  } else {
    *out_lease = 0;
  }

  return 1;
}

int dhcp_acquire(void) {
  if (!net_dev.have_mac) {
    uart_errorln("[DHCP] no MAC; can't run");
    return -1;
  }

  uart_println("[DHCP] Starting acquire...");

  static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t any_ip[4]    = {0, 0, 0, 0};
  static const uint8_t bcast_ip[4]  = {255, 255, 255, 255};

  /* Transaction id — fixed per acquire. Slirp doesn't enforce uniqueness,
   * so any non-zero value works for our purposes. */
  uint32_t xid = 0xFE221001u;

  uint8_t rx[600];
  uint8_t yiaddr[4]    = {0, 0, 0, 0};
  uint8_t server_id[4] = {0, 0, 0, 0};
  uint8_t mask[4]      = {255, 255, 255, 0};
  uint8_t router[4]    = {0, 0, 0, 0};
  uint32_t lease       = 0;

  /* ---- DISCOVER ---- */
  uint32_t blen = dhcp_build(bootp_buf, DHCP_DISCOVER, net_dev.mac, xid,
                             0, 0);
  uint32_t flen = udp_build(dhcp_frame, bcast_mac, any_ip, bcast_ip,
                            68, 67, bootp_buf, blen);
  if (net_tx(dhcp_frame, flen) <= 0) {
    uart_errorln("[DHCP] DISCOVER TX failed");
    return -1;
  }
  uart_println("[DHCP] DISCOVER sent");

  /* ---- Wait for OFFER ---- */
  int got = 0;
  for (uint32_t s = 0; s < 5000000u && !got; s++) {
    int n = net_rx_poll(rx, sizeof(rx));
    if (n > 0 && dhcp_parse(rx, (uint32_t)n, xid, DHCP_OFFER,
                            yiaddr, server_id, mask, router, &lease)) {
      got = 1;
    }
  }
  if (!got) {
    uart_errorln("[DHCP] no OFFER received");
    return -1;
  }
  uart_printf("[DHCP] OFFER: %d.%d.%d.%d (server=%d.%d.%d.%d, lease=%us)\n",
              (uint64_t)yiaddr[0], (uint64_t)yiaddr[1],
              (uint64_t)yiaddr[2], (uint64_t)yiaddr[3],
              (uint64_t)server_id[0], (uint64_t)server_id[1],
              (uint64_t)server_id[2], (uint64_t)server_id[3],
              lease);

  /* ---- REQUEST ---- */
  blen = dhcp_build(bootp_buf, DHCP_REQUEST, net_dev.mac, xid,
                    yiaddr, server_id);
  flen = udp_build(dhcp_frame, bcast_mac, any_ip, bcast_ip,
                   68, 67, bootp_buf, blen);
  if (net_tx(dhcp_frame, flen) <= 0) {
    uart_errorln("[DHCP] REQUEST TX failed");
    return -1;
  }
  uart_println("[DHCP] REQUEST sent");

  /* ---- Wait for ACK ---- */
  uint8_t y2[4]  = {0, 0, 0, 0};
  uint8_t s2[4]  = {0, 0, 0, 0};
  uint8_t m2[4]  = {255, 255, 255, 0};
  uint8_t r2[4]  = {0, 0, 0, 0};
  uint32_t l2    = 0;
  got = 0;
  for (uint32_t s = 0; s < 5000000u && !got; s++) {
    int n = net_rx_poll(rx, sizeof(rx));
    if (n > 0 && dhcp_parse(rx, (uint32_t)n, xid, DHCP_ACK,
                            y2, s2, m2, r2, &l2)) {
      got = 1;
    }
  }
  if (!got) {
    uart_errorln("[DHCP] no ACK received");
    return -1;
  }

  /* Commit lease to globals so net_send_arp_probe / net_send_ping /
   * net_get_info pick up the new values immediately. */
  for (int i = 0; i < 4; i++) g_my_ip[i]       = y2[i];
  for (int i = 0; i < 4; i++) g_subnet_mask[i] = m2[i];
  for (int i = 0; i < 4; i++) g_gateway_ip[i]  = r2[i];
  for (int i = 0; i < 4; i++) g_dhcp_server[i] = s2[i];
  g_lease_secs    = l2;
  g_dhcp_acquired = 1;

  uart_printf("[DHCP] Lease ACK \\o/  ip=%d.%d.%d.%d mask=%d.%d.%d.%d "
              "gw=%d.%d.%d.%d srv=%d.%d.%d.%d lease=%us\n",
              (uint64_t)g_my_ip[0], (uint64_t)g_my_ip[1],
              (uint64_t)g_my_ip[2], (uint64_t)g_my_ip[3],
              (uint64_t)g_subnet_mask[0], (uint64_t)g_subnet_mask[1],
              (uint64_t)g_subnet_mask[2], (uint64_t)g_subnet_mask[3],
              (uint64_t)g_gateway_ip[0], (uint64_t)g_gateway_ip[1],
              (uint64_t)g_gateway_ip[2], (uint64_t)g_gateway_ip[3],
              (uint64_t)g_dhcp_server[0], (uint64_t)g_dhcp_server[1],
              (uint64_t)g_dhcp_server[2], (uint64_t)g_dhcp_server[3],
              g_lease_secs);
  return 0;
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


  /* Acquire an IPv4 lease via DHCP before doing any IP-level work, so
   * subsequent ARP / ICMP frames carry the leased IP rather than the
   * hard-coded slirp default. */
  dhcp_acquire();


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
