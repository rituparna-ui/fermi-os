# VirtIO Network Driver (net) - Rust Porting Specification

## Overview

The net subsystem is the largest Fermi OS component, implementing a full VirtIO 1.x modern device driver for Ethernet networking on aarch64 QEMU virt. It provides:

- **Device Discovery & Initialization**: PCI enumeration, VirtIO negotiation, virtqueue setup per VirtIO 1.x §3.1.1
- **TX/RX Data Path**: Synchronous polling via dual virtqueues (RX=0, TX=1) with virtio_net_hdr prepending
- **RX Prefill**: 8 fixed 1600-byte buffers pre-armed on RX queue, automatically re-armed after drain
- **L3 Helpers**: RFC 1071 internet checksum, ARP reply parsing, ICMP echo request construction
- **DHCP Client**: RFC 2131 minimal four-step (DISCOVER→OFFER→REQUEST→ACK) over UDP/IPv4
- **Smoke Tests**: ARP probe, gateway MAC learning, ICMP ping round-trip via boot sequence
- **Statistics**: Per-direction packet counters (rx_packets, tx_packets) surfaced via /proc/netinfo
- **IPv4 State**: Global config (my_ip, subnet_mask, gateway_ip, dhcp_server, lease_secs, dhcp_acquired)

The implementation uses synchronous polling (no interrupts), making it suitable for boot-time networking before full scheduler/IRQ infrastructure is ready.

## Device IDs & Feature Bits

```
VIRTIO_NET_VENDOR_ID = 0x1AF4
VIRTIO_NET_DEVICE_ID = 0x1041  /* Modern, disable-legacy=on */

Device Feature Bits (low 32):
  VIRTIO_NET_F_MAC     = (1U << 5)   /* Device provides MAC in config */
  VIRTIO_NET_F_STATUS  = (1U << 16)  /* Device provides link status in config */

Device Feature Bits (high 32, feat_hi & 0x01):
  VIRTIO_F_VERSION_1   = bit 0 (required, must be advertised for modern path)

Negotiated Features:
  - Accept: VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS, VIRTIO_F_VERSION_1
  - Reject: All GSO/checksum-offload features (plain Ethernet only)
```

## Device Configuration Space (VirtIO 1.x §5.1.4)

All accesses through device_cfg MMIO base (discovered via PCI capabilities):

```
Offset  Width  Field                 Semantics
------  -----  -----                 ---------
0x00    6B     mac[0..5]             MAC address (if VIRTIO_NET_F_MAC negotiated)
0x06    2B     status                Link status bits (if VIRTIO_NET_F_STATUS negotiated)
```

**Link Status Bits** (config.status at offset 0x06):
```
0x0001  VIRTIO_NET_S_LINK_UP         Link is UP
0x0002  VIRTIO_NET_S_ANNOUNCE        Announce (not used)
```

## VirtIO Net Header (Prepended to Every Frame)

All TX and RX frames are prefixed with this header. Modern (VIRTIO_F_VERSION_1) path always uses the 12-byte form:

```c
struct virtio_net_hdr {
  uint8_t  flags;         /* Offset 0 */
  uint8_t  gso_type;      /* Offset 1 */
  uint16_t hdr_len;       /* Offset 2-3 (network order) */
  uint16_t gso_size;      /* Offset 4-5 (network order) */
  uint16_t csum_start;    /* Offset 6-7 (network order) */
  uint16_t csum_offset;   /* Offset 8-9 (network order) */
  uint16_t num_buffers;   /* Offset 10-11 (network order) */
} __attribute__((packed));

Size: 12 bytes (VIRTIO_NET_HDR_LEN = 12)
Alignment: 16 bytes (buffers must be 16-byte aligned in driver)
Byte order: Network (big-endian) for multi-byte fields
```

For plain Ethernet (no GSO, no checksum offload), TX fills all fields with 0.

## VirtIO Common Config Registers (§4.1.4.3)

All register offsets relative to common_cfg MMIO base:

```
Offset  Width  Name                      Access  Semantics
------  -----  ----                      ------  ---------
0x00    4B     DFSELECT                  rw      Device feature select (0-1)
0x04    4B     DF                        ro      Device feature value
0x08    4B     GFSELECT                  rw      Guest feature select (0-1)
0x0C    4B     GF                        rw      Guest feature (negotiation)
0x10    2B     MSIX                      rw      MSI-X config vector (0xFFFF = no vector)
0x12    2B     NUMQ                      ro      Number of queues
0x14    1B     STATUS                    rw      Device status bits
0x15    1B     CFGGEN                    ro      Config generation counter
0x16    2B     Q_SELECT                  rw      Queue select (0=RX, 1=TX)
0x18    2B     Q_SIZE                    rw      Queue size (max 256 for net, driver max 16)
0x1A    2B     Q_MSIX                    rw      Queue MSI-X vector (0xFFFF = polling)
0x1C    2B     Q_ENABLE                  rw      Queue enable flag
0x1E    2B     Q_NOFF                    ro      Queue notify offset (in notify multiplier units)
0x20    4B     Q_DESCLO                  rw      Descriptor table PA (bits 0-31)
0x24    4B     Q_DESCHI                  rw      Descriptor table PA (bits 32-63)
0x28    4B     Q_DRIVERLO                rw      Available ring PA (bits 0-31)
0x2C    4B     Q_DRIVERHI                rw      Available ring PA (bits 32-63)
0x30    4B     Q_DEVICELO                rw      Used ring PA (bits 0-31)
0x34    4B     Q_DEVICEHI                rw      Used ring PA (bits 32-63)
```

**Status Bits** (STATUS at 0x14):
```
0x00    VIRTIO_STATUS_RESET           Device reset (not negotiating)
0x01    VIRTIO_STATUS_ACKNOWLEDGE     Guest acknowledges device
0x02    VIRTIO_STATUS_DRIVER          Guest driver recognizes device
0x04    VIRTIO_STATUS_FEATURES_OK     Features negotiated successfully
0x08    VIRTIO_STATUS_DRIVER_OK       Driver ready to use device
0x80    VIRTIO_STATUS_FAILED          Device failed (fatal, unrecoverable)
```

## VirtQueue Structure & Ring Layout

### Virtqueue Descriptor Table

Fixed, page-aligned, 4096-byte-aligned array of descriptors (16 entries max for net):

```c
struct virtq_desc {
  uint64_t addr;        /* Physical address of buffer */
  uint32_t len;         /* Length in bytes */
  uint16_t flags;       /* VIRTQ_DESC_F_* bits */
  uint16_t next;        /* Index of next descriptor in chain (if NEXT flag) */
};

#define VIRTQ_DESC_F_NONE  0    /* Buffer is device-readable */
#define VIRTQ_DESC_F_NEXT  1    /* Buffer continues (linked) */
#define VIRTQ_DESC_F_WRITE 2    /* Buffer is device-writable (device→driver) */

Size per descriptor: 16 bytes
Max descriptors (VIRTQ_MAX_SIZE): 16
Total table size: 16 * 16 = 256 bytes (fits in single page with alignment padding)
```

### Available Ring (Driver → Device)

Tells device which descriptors contain data to process:

```c
struct virtq_avail {
  uint16_t flags;                   /* Offset 0-1 */
  uint16_t idx;                     /* Offset 2-3: next index driver will write to */
  uint16_t ring[VIRTQ_MAX_SIZE];    /* Offset 4-35: descriptor indices */
};

Size: 4 + 2*16 = 36 bytes (padded to page boundary)
```

### Used Ring (Device → Driver)

Device writes here after processing descriptors:

```c
struct virtq_used_elem {
  uint32_t id;          /* Descriptor index device finished */
  uint32_t len;         /* Bytes written by device */
};

struct virtq_used {
  uint16_t flags;                           /* Offset 0-1 */
  uint16_t idx;                             /* Offset 2-3: next index device will write to */
  struct virtq_used_elem ring[VIRTQ_MAX_SIZE]; /* Offset 4-...: completion records */
};

Size per element: 8 bytes
Total: 4 + 16*8 = 132 bytes (padded to page boundary)
```

### Virtqueue State (Driver-Side)

```c
struct virtqueue {
  uint16_t size;              /* Negotiated queue size (16 for net) */
  uint16_t free_head;         /* Next free descriptor index for allocation */
  uint16_t last_used;         /* Last used.idx we've processed */
  uintptr_t notify_addr;      /* PA of queue notification doorbell */
  struct virtq_desc *desc;    /* VA of descriptor table */
  struct virtq_avail *avail;  /* VA of available ring */
  struct virtq_used *used;    /* VA of used ring */
};
```

## Boot/Initialization Sequence (VirtIO 1.x §3.1.1)

**Call Path:** `kernel_main()` → `pci_virtio_net_init()` → DHCP, smoke tests

### Step 0: Device Discovery
- Enumerate PCI bus, find vendor 0x1AF4, device 0x1041
- Call `pci_assign_bars()` to map BARs to guest PA
- Call `pci_enable_device()` to set PCI_COMMAND bits
- Call `virtio_parse_capabilities()` to extract:
  - common_cfg: MMIO base for register access
  - notify_base: MMIO base for queue notification doorbells
  - device_cfg: MMIO base for device-specific config
  - notify_off_multiplier: scale factor for queue notify offsets

### Step 1: Reset Device
```c
mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
dsb_sy();  /* Data synchronization barrier */
while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) { }
```

### Step 2: ACK Device
```c
uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
dsb_sy();
```

### Step 3: Set DRIVER Status
```c
status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
dsb_sy();
```

### Step 4: Feature Negotiation
```c
/* Read device features (bits 0-31) */
mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
dsb_sy();
uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);

/* Read device features (bits 32-63) */
mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
dsb_sy();
uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);

/* Verify VIRTIO_F_VERSION_1 (bit 32 = feat_hi & 0x01) is present, else FAIL */
if (!(feat_hi & 0x01)) {
  uart_errorln("[NET] Device does not advertise VIRTIO_F_VERSION_1");
  return; /* FATAL */
}

/* Negotiate: accept MAC, STATUS, VERSION_1; reject GSO/checksum features */
uint32_t want_lo = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
uint32_t guest_lo = feat_lo & want_lo;
uint32_t guest_hi = feat_hi & 0x01;  /* VERSION_1 only */

/* Write negotiated features back */
mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
dsb_sy();
mmio_write32(base + VIRTIO_COMMON_GF, guest_lo);
dsb_sy();
mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
dsb_sy();
mmio_write32(base + VIRTIO_COMMON_GF, guest_hi);
dsb_sy();
```

### Step 5: FEATURES_OK
```c
status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
dsb_sy();

/* Verify FEATURES_OK bit sticks */
status = mmio_read8(base + VIRTIO_COMMON_STATUS);
if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
  uart_errorln("[NET] FEATURES_OK failed");
  return; /* FATAL */
}
```

### Step 6: Setup Virtqueues (RX then TX)

For each queue (RX=0, TX=1):
```c
/* Select queue */
mmio_write16(base + VIRTIO_COMMON_Q_SELECT, queue_idx);

/* Set queue size (via virtqueue_setup, which negotiates) */
uint16_t max_size = mmio_read16(base + VIRTIO_COMMON_Q_SIZE);
/* For net, max_size ≥ 16; driver caps at VIRTQ_MAX_SIZE=16 */
mmio_write16(base + VIRTIO_COMMON_Q_SIZE, 16);

/* Write descriptor table PA (64-bit, must be page-aligned) */
uint64_t desc_pa = VIRT_TO_PHYS((uint64_t)&desc_table);
mmio_write32(base + VIRTIO_COMMON_Q_DESCLO, (uint32_t)desc_pa);
mmio_write32(base + VIRTIO_COMMON_Q_DESCHI, (uint32_t)(desc_pa >> 32));

/* Write available ring PA (page-aligned) */
uint64_t avail_pa = VIRT_TO_PHYS((uint64_t)&avail_ring);
mmio_write32(base + VIRTIO_COMMON_Q_DRIVERLO, (uint32_t)avail_pa);
mmio_write32(base + VIRTIO_COMMON_Q_DRIVERHI, (uint32_t)(avail_pa >> 32));

/* Write used ring PA (page-aligned) */
uint64_t used_pa = VIRT_TO_PHYS((uint64_t)&used_ring);
mmio_write32(base + VIRTIO_COMMON_Q_DEVICELO, (uint32_t)used_pa);
mmio_write32(base + VIRTIO_COMMON_Q_DEVICEHI, (uint32_t)(used_pa >> 32));

/* Set MSI-X to no vector (polling mode) */
mmio_write16(base + VIRTIO_COMMON_Q_MSIX, 0xFFFF);

/* Enable queue */
mmio_write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
```

### Step 7: DRIVER_OK
```c
status = mmio_read8(base + VIRTIO_COMMON_STATUS);
mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
dsb_sy();
```

### Step 8: RX Prefill
Immediately after DRIVER_OK, arm 8 × 1600-byte buffers on RX queue via `virtqueue_submit()` with VIRTQ_DESC_F_WRITE, then `virtqueue_notify()`.

### Step 9: Read Device Config & Boot Tests
- Read MAC from device_cfg offset 0x00 (6 bytes) if VIRTIO_NET_F_MAC negotiated
- Read link status from device_cfg offset 0x06 (2 bytes) if VIRTIO_NET_F_STATUS negotiated
- Run DHCP acquire to populate global IPv4 state
- Send ARP probe, poll RX for reply (learn gateway MAC)
- Send ICMP ping, poll RX for echo reply

## Public API Functions

### TX Path

```c
int net_tx(const void *frame, uint32_t len);
```

**Behavior:**
- Sends a raw Ethernet frame (destination MAC + source MAC + ethertype + payload)
- Prepends virtio_net_hdr internally; caller does not include it
- Synchronous: blocks polling until device ACKs via used ring
- Returns number of bytes accepted on success (== len), or negative on error
- Increments tx_packets counter on success

**Errors:**
- Returns -1 if frame is NULL or len is 0

**Frame Format (Caller Provides):**
```
[Ethernet Dst MAC: 6B]
[Ethernet Src MAC: 6B]
[Ethertype: 2B, network order]
[Payload: variable, len-14 bytes]
```

Total len must include all 14 bytes of Ethernet header.

### RX Path

```c
int net_rx_poll(void *dst, uint32_t max_len);
```

**Behavior:**
- Non-blocking poll: checks used ring for completed RX buffers
- On match: copies payload (skipping virtio_net_hdr) into dst, up to max_len bytes
- Automatically re-arms the same RX buffer on the virtqueue for reuse
- Increments rx_packets counter if copied > 0
- Returns:
  - > 0: number of payload bytes copied (excluding 12-byte virtio_net_hdr)
  - 0: nothing pending (nothing in used ring)
  - < 0: error (e.g., unmapped descriptor, uninitialized RX queue)

**Preconditions:**
- RX queue must have been prefilled via net_rx_init() (called by pci_virtio_net_init)

**Buffer Management:**
- Caller-provided dst must be ≥ max_len bytes
- If dst is NULL and copied > 0, data is discarded (still increments counter, still re-arms)

### DHCP Client

```c
int dhcp_acquire(void);
```

**Behavior:**
- Performs RFC 2131 DISCOVER → OFFER → REQUEST → ACK four-message exchange
- On success (ACK received): commits lease into globals (g_my_ip, g_subnet_mask, g_gateway_ip, g_dhcp_server, g_lease_secs, g_dhcp_acquired = 1) and returns 0
- On failure (no reply within timeout, or NAK): leaves globals untouched, returns -1
- Synchronous: blocks polling RX with ~5 million spin cap per step (waits for DHCP server reply)
- Requires MAC already negotiated (net_dev.have_mac must be true, set by Step 9 of pci_virtio_net_init)

**Globals Updated on Success:**
```c
uint8_t  g_my_ip[4]       /* Leased IPv4 address */
uint8_t  g_subnet_mask[4] /* Subnet mask */
uint8_t  g_gateway_ip[4]  /* Default gateway */
uint8_t  g_dhcp_server[4] /* DHCP server address */
uint32_t g_lease_secs     /* Lease duration in seconds */
uint8_t  g_dhcp_acquired  /* 1 if acquired, else 0 */
```

**DHCP Message Types:**
```
DHCP_DISCOVER = 1  /* Guest → Server: request address */
DHCP_OFFER    = 2  /* Server → Guest: offer address */
DHCP_REQUEST  = 3  /* Guest → Server: accept offer */
DHCP_ACK      = 5  /* Server → Guest: lease confirmed */
DHCP_NAK      = 6  /* Server → Guest: request rejected (not handled) */
```

**UDP Stack:**
- Uses broadcast MAC (ff:ff:ff:ff:ff:ff) for DISCOVER/REQUEST
- Uses 0.0.0.0 as source IP for DISCOVER/REQUEST
- Uses 255.255.255.255 as destination IP
- Source port: 68 (DHCP client), Destination port: 67 (DHCP server)
- UDP checksum left at 0 (legal for IPv4)

### ARP Probe (Smoke Test)

```c
int net_send_arp_probe(void);
```

**Behavior:**
- Constructs and sends a 60-byte ARP request asking "who has 10.0.2.2?" (slirp gateway)
- Destination is broadcast (ff:ff:ff:ff:ff:ff)
- Sender hardware address is net_dev.mac
- Sender protocol address is g_my_ip (default 10.0.2.15)
- Target protocol address is 10.0.2.2
- Returns bytes sent on success (60), or -1 on error (no MAC)

**Errors:**
- Returns -1 if net_dev.have_mac is false

**ARP Frame Layout:**
```
[Ethernet Dst MAC: ff:ff:ff:ff:ff:ff (6B)]
[Ethernet Src MAC: net_dev.mac (6B)]
[Ethertype: 0x0806 ARP (2B)]
[HTYPE: 0x0001 Ethernet (2B)]
[PTYPE: 0x0800 IPv4 (2B)]
[HLEN: 6 (1B)]
[PLEN: 4 (1B)]
[OPER: 0x0001 request (2B)]
[SHA (sender HW addr): net_dev.mac (6B)]
[SPA (sender protocol): g_my_ip (4B)]
[THA (target HW addr): 0x00... (6B, zero for request)]
[TPA (target protocol): g_gateway_ip (4B)]
Total: 60 bytes (Ethernet minimum)
```

### ICMP Ping (Smoke Test)

```c
int net_send_ping(uint16_t seq);
```

**Behavior:**
- Constructs and sends an ICMP echo request to 10.0.2.2 (slirp gateway)
- Destination MAC is learned gateway_mac (set by parse_arp_reply)
- Source IP is g_my_ip, destination IP is g_gateway_ip
- ICMP identifier is fixed at 42, sequence number is caller-provided
- Payload is 56 bytes of repeated ASCII pattern ('a'+i%26, i=0..55)
- Returns bytes sent on success (98: 14 Eth + 20 IP + 8 ICMP + 56 payload), or -1 on error

**Errors:**
- Returns -1 if have_gateway_mac is false (run ARP probe first to learn gateway MAC)

**Frame Layout:**
```
[Ethernet Dst MAC: gateway_mac (6B)]
[Ethernet Src MAC: net_dev.mac (6B)]
[Ethertype: 0x0800 IPv4 (2B)]
[IPv4 header (20B):
  Version/IHL: 0x45
  TOS: 0
  Total Length: 20+8+56 = 84 bytes (network order)
  ID: 0
  Flags/Frag: 0
  TTL: 64
  Protocol: 1 (ICMP)
  Checksum: computed via inet_csum() (network order)
  Src IP: g_my_ip (4B)
  Dst IP: g_gateway_ip (4B)]
[ICMP echo request header (8B):
  Type: 8 (echo request)
  Code: 0
  Checksum: computed via inet_csum() (network order)
  Identifier: 42 (fixed)
  Sequence: seq (network order)]
[ICMP payload (56B): 'a' 'b' ... 'z' 'a' ... ]
Total: 98 bytes
```

### Device Info Snapshot

```c
int net_get_info(char *buf, uint32_t buflen);
```

**Behavior:**
- Renders a multi-line /proc-style snapshot of device state into buf
- Caller-allocated buffer; truncates safely if buflen too small
- Returns total bytes written (may exceed buflen if truncated)

**Output Format:**
```
mac:        xx:xx:xx:xx:xx:xx
link:       UP | DOWN
ip:         d.d.d.d
netmask:    d.d.d.d
gateway:    d.d.d.d
dhcp:       yes | no
dhcp_srv:   d.d.d.d
lease:      N s
gw_mac:     xx:xx:xx:xx:xx:xx | (unknown)
rx_packets: N
tx_packets: N
```

### Initialization

```c
void pci_virtio_net_init(void);
```

**Behavior:**
- Main entry point, called once from kernel_main() during boot
- Performs complete VirtIO initialization (Steps 0-7 above)
- Calls net_rx_init() to prefill RX queue
- Reads MAC and link status from device config
- Runs DHCP acquire
- Smoke-tests wire: ARP probe → learn gateway MAC → ICMP ping
- Prints diagnostics to UART throughout
- Never returns (fatal on device not found or init failure)

## Global State

**Per-Direction Counters** (static, not exported):
```c
static uint64_t rx_packets;  /* Incremented on each net_rx_poll() success */
static uint64_t tx_packets;  /* Incremented on each net_tx() success */
```

**IPv4 + DHCP State** (exported, read by ARP/ICMP/get_info):
```c
uint8_t  g_my_ip[4]       = {10, 0, 2, 15};  /* Leased IPv4 */
uint8_t  g_subnet_mask[4] = {255, 255, 255, 0};
uint8_t  g_gateway_ip[4]  = {10, 0, 2, 2};
uint8_t  g_dhcp_server[4] = {0, 0, 0, 0};
uint32_t g_lease_secs     = 0;
uint8_t  g_dhcp_acquired  = 0;
```

**Device State** (static):
```c
static struct virtio_net net_dev {
  struct pci_device      pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue       rx_vq;
  struct virtqueue       tx_vq;
  uint8_t                mac[6];
  uint8_t                have_mac;
  uint16_t               link_status;
  uint8_t                have_status;
};
```

**RX Queue Internals** (static):
```c
#define NET_RX_BUF_COUNT 8       /* Number of pre-allocated RX buffers */
#define NET_RX_BUF_SIZE  1600    /* Bytes per buffer (12 hdr + 1500 MTU + slack) */

static uint8_t rx_bufs[NET_RX_BUF_COUNT][NET_RX_BUF_SIZE]
    __attribute__((aligned(64)));  /* Data buffers, 64-byte aligned */
static int     rx_desc_to_buf[VIRTQ_MAX_SIZE];  /* Map desc index → buf index, or -1 */
static uint8_t rx_initialized;  /* 1 after net_rx_init() called */
```

**TX Header Buffer** (static):
```c
static struct virtio_net_hdr tx_hdr __attribute__((aligned(16)));
```

**Learned Gateway MAC** (static, set by parse_arp_reply):
```c
static uint8_t gateway_mac[6];
static uint8_t have_gateway_mac;
```

## Helper Functions (Static, Not Public)

### RFC 1071 Internet Checksum

```c
static uint16_t inet_csum(const uint8_t *data, uint32_t len);
```

Computes RFC 1071 checksum: one's complement sum of all 16-bit words (network order), then complement. Returns network-order result.

### ARP Reply Parser

```c
static void parse_arp_reply(const uint8_t *frame, uint32_t len);
```

If frame is an ARP reply for our query, extracts sender MAC into gateway_mac and sets have_gateway_mac=1. Otherwise no-op.

### UDP Frame Builder

```c
static uint32_t udp_build(uint8_t *frame,
                          const uint8_t dst_mac[6],
                          const uint8_t src_ip[4],
                          const uint8_t dst_ip[4],
                          uint16_t src_port, uint16_t dst_port,
                          const uint8_t *payload, uint32_t payload_len);
```

Builds complete Ethernet/IPv4/UDP frame around payload. Returns total frame length.

### DHCP Frame Builder

```c
static uint32_t dhcp_build(uint8_t *bootp, uint8_t msg_type,
                           const uint8_t client_mac[6], uint32_t xid,
                           const uint8_t *requested_ip,
                           const uint8_t *server_id);
```

Builds DHCP BOOTP section (240 bytes fixed + options). Returns total length.

### DHCP Option Parser

```c
static const uint8_t *dhcp_find_option(const uint8_t *opts, uint32_t max_len,
                                       uint8_t want, uint8_t *found_len);
```

Finds DHCP option code in options blob. Stores length in *found_len, returns pointer to value bytes or NULL if missing.

### DHCP Reply Validator

```c
static int dhcp_parse(const uint8_t *frame, uint32_t flen,
                      uint32_t expect_xid, uint8_t expect_msg,
                      uint8_t out_yiaddr[4], uint8_t out_server[4],
                      uint8_t out_mask[4],   uint8_t out_router[4],
                      uint32_t *out_lease);
```

Validates frame is a DHCP reply with matching XID and message type. Extracts offered IP, server, netmask, router, lease. Returns 1 on match (out_* filled), 0 otherwise.

### RX Queue Initialization

```c
static void net_rx_init(void);
```

Prefills RX queue with 8 buffers, arms them, and notifies device. Sets rx_initialized=1.

### RX Buffer Submit

```c
static void net_rx_submit_buf(int buf_idx);
```

Arms a single RX buffer on the virtqueue at the next free descriptor index, updating rx_desc_to_buf[] mapping. Called after draining a used buffer to re-arm it.

## Data Structures (Exact Layouts)

### virtio_net_hdr (12 bytes, packed, 16-byte aligned buffers)
```
Offset  Size  Field
0       1     flags
1       1     gso_type
2       2     hdr_len
4       2     gso_size
6       2     csum_start
8       2     csum_offset
10      2     num_buffers
```

### virtq_desc (16 bytes)
```
Offset  Size  Field
0       8     addr
8       4     len
12      2     flags
14      2     next
```

### virtq_avail (4 + 2*16 = 36 bytes, page-padded)
```
Offset  Size      Field
0       2         flags
2       2         idx
4       32        ring[16]
```

### virtq_used_elem (8 bytes each)
```
Offset  Size  Field
0       4     id
4       4     len
```

### virtq_used (4 + 8*16 = 132 bytes, page-padded)
```
Offset  Size      Field
0       2         flags
2       2         idx
4       128       ring[16] (each 8 bytes)
```

## Hardware Constants & Register Offsets

**VirtIO Common Config Base Offsets:**
- VIRTIO_COMMON_DFSELECT = 0x00
- VIRTIO_COMMON_DF = 0x04
- VIRTIO_COMMON_GFSELECT = 0x08
- VIRTIO_COMMON_GF = 0x0C
- VIRTIO_COMMON_MSIX = 0x10
- VIRTIO_COMMON_NUMQ = 0x12
- VIRTIO_COMMON_STATUS = 0x14
- VIRTIO_COMMON_CFGGEN = 0x15
- VIRTIO_COMMON_Q_SELECT = 0x16
- VIRTIO_COMMON_Q_SIZE = 0x18
- VIRTIO_COMMON_Q_MSIX = 0x1A
- VIRTIO_COMMON_Q_ENABLE = 0x1C
- VIRTIO_COMMON_Q_NOFF = 0x1E
- VIRTIO_COMMON_Q_DESCLO = 0x20
- VIRTIO_COMMON_Q_DESCHI = 0x24
- VIRTIO_COMMON_Q_DRIVERLO = 0x28
- VIRTIO_COMMON_Q_DRIVERHI = 0x2C
- VIRTIO_COMMON_Q_DEVICELO = 0x30
- VIRTIO_COMMON_Q_DEVICEHI = 0x34

**VirtIO Net Config Space Offsets:**
- VIRTIO_NET_CFG_MAC = 0x00 (6 bytes)
- VIRTIO_NET_CFG_STATUS = 0x06 (2 bytes)

**Queue Indices:**
- VIRTIO_NET_QUEUE_RX = 0
- VIRTIO_NET_QUEUE_TX = 1

**Max Sizes:**
- VIRTQ_MAX_SIZE = 16 (driver limit, independent of device capability)
- NET_RX_BUF_COUNT = 8
- NET_RX_BUF_SIZE = 1600

**DHCP Constants:**
- DHCP_MAGIC_0/1/2/3 = 0x63, 0x82, 0x53, 0x63
- DHCP_OPT_SUBNET = 1
- DHCP_OPT_ROUTER = 3
- DHCP_OPT_REQ_IP = 50
- DHCP_OPT_LEASE = 51
- DHCP_OPT_MSGTYPE = 53
- DHCP_OPT_SERVER_ID = 54
- DHCP_OPT_PARAM_REQ = 55
- DHCP_OPT_END = 255

**ICMP:**
- ICMP_PING_PAYLOAD = 56 bytes
- ICMP echo request type = 8
- ICMP echo reply type = 0

**Ethernet:**
- VIRTIO_NET_HDR_LEN = 12
- Ethernet header = 14 bytes (6 dst + 6 src + 2 ethertype)
- IPv4 header = 20 bytes (minimum IHL=5)
- UDP header = 8 bytes
- ICMP header = 8 bytes
- ARP header = 28 bytes

**Memory Alignments:**
- Descriptor tables, rings: 4096-byte page alignment
- RX buffers: 64-byte alignment
- TX header: 16-byte alignment
- Virtio_net_hdr: 16-byte aligned buffers in TX/RX paths

## Critical Ordering & Gotchas

### Barrier Requirements
- **dsb_sy()** (Data Synchronization Barrier, inner shareable) must follow:
  - Every MMIO write that affects state (status, features, queue addresses, enables)
  - Before reading status to verify state changes (FEATURES_OK, DRIVER_OK stuck bits)
  - After reading used ring to ensure device writes are visible
  - Pattern: write, dsb_sy(), then dependent read/write
  
### Volatile Semantics
- Used ring index (used->idx) must be read as volatile; device writes it asynchronously
- Pattern: `uint16_t used_now = *(volatile uint16_t *)&net_dev.rx_vq.used->idx;`

### Descriptor Allocation Order
- free_head is incremented linearly; no wraparound tracking yet in C code
- Chains must fit: submitted chain of N segments uses N consecutive descriptors starting at free_head
- If free_head + N > VIRTQ_MAX_SIZE, chain wraps (must handle in Rust)

### RX Buffer Lifecycle
- Buffers pre-armed on RX queue survive across multiple polls
- rx_desc_to_buf[desc_id] tracks which buffer is at each descriptor
- After poll drains a used entry: must call net_rx_submit_buf(buf_idx) to re-arm same buffer
- Missing re-arm = RX queue gradually drains and stalls

### TX Synchronous Polling
- net_tx() calls virtqueue_poll(), which spins until device ACKs
- No timeout; device must respond or boot hangs
- Global tx_hdr is safe to reuse because net_tx() is synchronous (no concurrent calls)

### DHCP Transaction ID
- xid = 0xFE221001 is hardcoded; slirp doesn't enforce uniqueness
- Must match in OFFER and ACK for validation

### Feature Negotiation Sequence
1. Read device features (both lo/hi)
2. Check VIRTIO_F_VERSION_1 (required) or FAIL
3. Mask to desired features, write back
4. Set FEATURES_OK status
5. Re-read status and verify FEATURES_OK bit is present; if not, FAIL
6. Only then proceed to queue setup

Reversing this order or missing re-check = device can reject and enter failed state.

### Physical Address Encoding
- 64-bit physical addresses split across two 32-bit MMIO writes
- Lo write first (0x20, 0x28, 0x30), then Hi write (0x24, 0x2C, 0x34)
- VIRT_TO_PHYS macro: `va - KERNEL_VA_OFFSET` (upper-half kernel identity map)

### Page Alignment Strict
- Descriptor table, available ring, used ring must each be page-aligned (4096-byte boundary)
- RX buffers are 64-byte aligned (fits on same page)
- If not aligned: device behavior undefined, will likely fault/hang

## Rust Module/Type Design

### Suggested Module Structure

```rust
pub mod net {
  pub mod driver {
    /* Main device driver state and initialization */
    pub struct VirtioNet { ... }
    impl VirtioNet {
      pub fn init() -> Result<Self, Error>
      pub fn tx(&mut self, frame: &[u8]) -> Result<usize, Error>
      pub fn rx_poll(&mut self, dst: &mut [u8]) -> Result<usize, Error>
    }
  }

  pub mod dhcp {
    /* DHCP protocol implementation */
    pub fn acquire(net: &mut VirtioNet) -> Result<DhcpLease, Error>
  }

  pub mod l3 {
    /* ARP, ICMP, IPv4 helpers */
    pub fn send_arp_probe(net: &mut VirtioNet) -> Result<usize, Error>
    pub fn send_ping(net: &mut VirtioNet, seq: u16) -> Result<usize, Error>
    pub fn inet_csum(data: &[u8]) -> u16
  }

  pub mod info {
    /* /proc-style device info snapshot */
    pub fn get_info(net: &VirtioNet, buf: &mut [u8]) -> usize
  }
}
```

### Key Types

```rust
#[repr(C, packed)]
pub struct VirtioNetHdr {
  flags: u8,
  gso_type: u8,
  hdr_len: u16,
  gso_size: u16,
  csum_start: u16,
  csum_offset: u16,
  num_buffers: u16,  // 12 bytes total
}

#[repr(C, packed)]
pub struct VirtqDesc {
  addr: u64,
  len: u32,
  flags: u16,
  next: u16,
}

pub struct Virtqueue {
  size: u16,
  free_head: u16,
  last_used: u16,
  notify_addr: u64,
  desc: &'static mut [VirtqDesc; VIRTQ_MAX_SIZE],
  avail: &'static mut VirtqAvail,
  used: &'static mut VirtqUsed,
}

pub struct VirtioNet {
  pci_dev: PciDevice,
  pci_caps: VirtioPciCaps,
  rx_vq: Virtqueue,
  tx_vq: Virtqueue,
  mac: [u8; 6],
  have_mac: bool,
  link_status: u16,
  have_status: bool,
  rx_bufs: [[u8; 1600]; 8],  // 64-byte aligned
  rx_desc_to_buf: [i32; VIRTQ_MAX_SIZE],
  rx_initialized: bool,
  tx_hdr: VirtioNetHdr,  // 16-byte aligned
  gateway_mac: [u8; 6],
  have_gateway_mac: bool,
}

pub struct DhcpLease {
  my_ip: [u8; 4],
  subnet_mask: [u8; 4],
  gateway_ip: [u8; 4],
  dhcp_server: [u8; 4],
  lease_secs: u32,
}
```

### Ownership & Locking Strategy

- **VirtioNet**: Single mutable instance, accessed only from kernel (EL1) context
  - No Arc/Mutex needed (no preemption, single-threaded during boot)
  - Store as static mut in net module, access via unsafe getter during init
  - Netd kernel task and user-space net syscalls coordinate via syscall ABI (no shared memory)

- **Statics**: Global IPv4 state (my_ip, subnet_mask, gateway_ip, etc.) must be:
  - Declared as public, accessed read-only from L3 helpers
  - Updated atomically by dhcp_acquire() (single write at end)
  - Safe: boot-time sequential (no concurrent DHCP calls during kernel init)

- **RX Buffers**: Per-descriptor tracking (rx_desc_to_buf) is kernel-private; no concurrency issues

### Pure-Rust vs. Assembly

**Must Stay Assembly (aarch64 asm):**
- `dsb_sy()`: Inline asm for DSB SY barrier
  ```rust
  #[inline]
  unsafe fn dsb_sy() {
    asm!("dsb sy" ::: "memory" : "volatile");
  }
  ```
- MMIO access may be inlined in mmio module (volatile reads/writes)

**Can Be Rust:**
- All protocol logic (DHCP parsing, ARP construction, ICMP checksums, etc.)
- Virtqueue ring management (pointer arithmetic within bounds)
- PCI cap parsing (already cross-module, no asm needed)
- Device initialization sequence (register writes via mmio module, barriers explicit)

### Features & Dependencies

- **core**: Yes (no_std kernel)
- **alloc**: Yes (for String, Vec in diagnostics; gate DHCP parser)
- **std**: No
- **Dependencies**: pci (device discovery), mmio (register access), uart (logging)
- **Barrier Semantics**: Explicit dsb_sy() calls at every synchronization point (no automatic barriers in Rust)

## Summary

The net subsystem is a complete VirtIO 1.x driver stack: device discovery and initialization (VirtIO §3.1.1), dual virtqueues for TX/RX, RFC 1071 checksums, DHCP client, ARP/ICMP helpers, and packet counters. All operations are synchronous polling (no interrupts), suitable for early-boot networking before scheduler/IRQ infrastructure. Critical gotchas include strict page alignment for ring structures, explicit DSB barriers after MMIO writes, volatile reads of asynchronously-written used ring indices, and proper re-arming of RX buffers after drain. The port should preserve exact magic numbers, register offsets, DHCP constants, and struct layouts; implement explicit barriers; and use Rust's type system to enforce alignment (newtype wrappers, repr(C, align)).
