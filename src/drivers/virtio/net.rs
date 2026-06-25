//! VirtIO network driver (`virtio-net-pci`, device id 0x1041) + a minimal L2/L3
//! stack: raw Ethernet TX/RX, ARP, IPv4, ICMP echo, and a synchronous DHCP
//! client. Frames are built byte-by-byte in network order; the device prepends
//! nothing the caller sees beyond the 12-byte virtio_net_hdr (handled here).

use crate::arch::cpu::dsb_sy;
use crate::drivers::pci;
use crate::drivers::virtio::virtqueue::{
    VirtqAvail, VirtqDesc, VirtqSeg, VirtqUsed, VirtqUsedElem, Virtqueue, VIRTQ_DESC_F_NONE,
    VIRTQ_DESC_F_WRITE, VIRTQ_MAX_SIZE,
};
use crate::drivers::virtio::*;
use crate::klib::fmtbuf::FmtBuf;
use crate::klib::sync::SpinLock;
use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::virt_to_phys;
use core::fmt::Write;

const VIRTIO_NET_VENDOR_ID: u16 = 0x1AF4;
const VIRTIO_NET_DEVICE_ID: u16 = 0x1041;
const PCI_ENDPOINT_DEV_TYPE: u8 = 0x00;

const CFG_MAC: usize = 0x00;
const CFG_STATUS: usize = 0x06;
const S_LINK_UP: u16 = 1 << 0;
const F_MAC: u32 = 1 << 5;
const F_STATUS: u32 = 1 << 16;

const NET_HDR_LEN: u32 = 12;
const QUEUE_RX: u16 = 0;
const QUEUE_TX: u16 = 1;

const RX_BUF_COUNT: usize = 8;
const RX_BUF_SIZE: usize = 1600; // 12B hdr + 1500 MTU + slack

// --- Static DMA storage (page-aligned rings + buffers, kernel VA) -----------

#[repr(C, align(4096))]
struct RingDesc([VirtqDesc; VIRTQ_MAX_SIZE]);
#[repr(C, align(4096))]
struct RingAvail(VirtqAvail);
#[repr(C, align(4096))]
struct RingUsed(VirtqUsed);
#[repr(align(64))]
struct Align64<T>(T);
#[repr(C, align(16))]
struct Align16<T>(T);

const DESC_INIT: RingDesc = RingDesc(
    [VirtqDesc {
        addr: 0,
        len: 0,
        flags: 0,
        next: 0,
    }; VIRTQ_MAX_SIZE],
);
const AVAIL_INIT: RingAvail = RingAvail(VirtqAvail {
    flags: 0,
    idx: 0,
    ring: [0; VIRTQ_MAX_SIZE],
});
const USED_INIT: RingUsed = RingUsed(VirtqUsed {
    flags: 0,
    idx: 0,
    ring: [VirtqUsedElem { id: 0, len: 0 }; VIRTQ_MAX_SIZE],
});

static mut RX_DESC: RingDesc = DESC_INIT;
static mut RX_AVAIL: RingAvail = AVAIL_INIT;
static mut RX_USED: RingUsed = USED_INIT;
static mut TX_DESC: RingDesc = DESC_INIT;
static mut TX_AVAIL: RingAvail = AVAIL_INIT;
static mut TX_USED: RingUsed = USED_INIT;
static mut TX_HDR: Align16<[u8; 12]> = Align16([0; 12]);
static mut RX_BUFS: Align64<[[u8; RX_BUF_SIZE]; RX_BUF_COUNT]> =
    Align64([[0; RX_BUF_SIZE]; RX_BUF_COUNT]);

/// Device + protocol state.
struct NetDevice {
    rx_vq: Virtqueue,
    tx_vq: Virtqueue,
    mac: [u8; 6],
    have_mac: bool,
    link_status: u16,
    rx_desc_to_buf: [i32; VIRTQ_MAX_SIZE],
    rx_initialized: bool,
    rx_packets: u64,
    tx_packets: u64,
    gateway_mac: [u8; 6],
    have_gateway_mac: bool,
    // IPv4 / DHCP state (slirp defaults until DHCP runs).
    my_ip: [u8; 4],
    subnet_mask: [u8; 4],
    gateway_ip: [u8; 4],
    dhcp_server: [u8; 4],
    lease_secs: u32,
    dhcp_acquired: bool,
    ready: bool,
}

// SAFETY: rings + buffers owned by this driver, accessed only under the lock.
unsafe impl Send for NetDevice {}

static NET: SpinLock<Option<NetDevice>> = SpinLock::new(None);

// --- Low-level TX/RX --------------------------------------------------------

fn tx_locked(dev: &mut NetDevice, frame: &[u8]) -> i64 {
    if frame.is_empty() {
        return -1;
    }
    // SAFETY: TX_HDR is this driver's DMA scratch, serialized by the lock.
    unsafe {
        let hdr = core::ptr::addr_of_mut!(TX_HDR.0) as *mut u8;
        core::ptr::write_bytes(hdr, 0, 12);
        let segs = [
            VirtqSeg {
                pa: virt_to_phys(hdr as u64),
                len: NET_HDR_LEN,
                flags: VIRTQ_DESC_F_NONE,
            },
            VirtqSeg {
                pa: virt_to_phys(frame.as_ptr() as u64),
                len: frame.len() as u32,
                flags: VIRTQ_DESC_F_NONE,
            },
        ];
        dev.tx_vq.submit_chain(&segs);
        dev.tx_vq.notify();
        dev.tx_vq.poll();
    }
    dev.tx_packets += 1;
    frame.len() as i64
}

fn rx_submit_buf(dev: &mut NetDevice, buf_idx: usize) {
    let desc_id = dev.rx_vq.free_head;
    // SAFETY: RX_BUFS is this driver's DMA region.
    let pa = unsafe { virt_to_phys(core::ptr::addr_of!(RX_BUFS.0[buf_idx]) as u64) };
    dev.rx_vq.submit(pa, RX_BUF_SIZE as u32, VIRTQ_DESC_F_WRITE);
    dev.rx_desc_to_buf[desc_id as usize] = buf_idx as i32;
}

fn rx_init(dev: &mut NetDevice) {
    for i in 0..VIRTQ_MAX_SIZE {
        dev.rx_desc_to_buf[i] = -1;
    }
    for i in 0..RX_BUF_COUNT {
        rx_submit_buf(dev, i);
    }
    dev.rx_vq.notify();
    dev.rx_initialized = true;
    kprintln!(
        "[NET] RX queue primed with {} buffers ({} bytes each)",
        RX_BUF_COUNT,
        RX_BUF_SIZE
    );
}

fn rx_poll_locked(dev: &mut NetDevice, dst: &mut [u8]) -> i64 {
    if !dev.rx_initialized {
        return -1;
    }
    let used_now = unsafe { core::ptr::read_volatile(&(*dev.rx_vq.used).idx) };
    if dev.rx_vq.last_used == used_now {
        return 0;
    }
    dsb_sy();

    let slot = (dev.rx_vq.last_used % dev.rx_vq.size) as usize;
    let (desc_id, total_len) = unsafe {
        // Volatile read of the device-written used element (dsb_sy orders the
        // CPU but isn't a compiler barrier; a plain deref could be cached).
        let e = core::ptr::read_volatile(&(*dev.rx_vq.used).ring[slot]);
        (e.id, e.len)
    };
    dev.rx_vq.last_used = dev.rx_vq.last_used.wrapping_add(1);

    if desc_id as usize >= VIRTQ_MAX_SIZE || dev.rx_desc_to_buf[desc_id as usize] < 0 {
        Uart.errorln("[NET] rx_poll: bogus / unmapped descriptor");
        return -1;
    }
    let buf_idx = dev.rx_desc_to_buf[desc_id as usize] as usize;
    dev.rx_desc_to_buf[desc_id as usize] = -1;

    let mut copied = 0i64;
    if total_len >= NET_HDR_LEN {
        let frame_len = (total_len - NET_HDR_LEN) as usize;
        let to_copy = core::cmp::min(frame_len, dst.len());
        if to_copy > 0 {
            // SAFETY: RX_BUFS[buf_idx] holds the device-written frame.
            unsafe {
                let src = (core::ptr::addr_of!(RX_BUFS.0[buf_idx]) as *const u8)
                    .add(NET_HDR_LEN as usize);
                core::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), to_copy);
            }
        }
        copied = to_copy as i64;
    }

    rx_submit_buf(dev, buf_idx);
    dev.rx_vq.notify();
    if copied > 0 {
        dev.rx_packets += 1;
    }
    copied
}

// --- Public TX/RX wrappers --------------------------------------------------

pub fn tx(frame: &[u8]) -> i64 {
    let mut g = NET.lock();
    match g.as_mut() {
        Some(d) if d.ready => tx_locked(d, frame),
        _ => -1,
    }
}

pub fn rx_poll(dst: &mut [u8]) -> i64 {
    let mut g = NET.lock();
    match g.as_mut() {
        Some(d) if d.ready => rx_poll_locked(d, dst),
        _ => -1,
    }
}

// --- L3: checksum, ARP, ICMP ------------------------------------------------

/// RFC 1071 internet checksum (network-order in, network-order out).
fn inet_csum(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let mut i = 0;
    while i + 1 < data.len() {
        sum += ((data[i] as u32) << 8) | data[i + 1] as u32;
        i += 2;
    }
    if i < data.len() {
        sum += (data[i] as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

fn parse_arp_reply(dev: &mut NetDevice, frame: &[u8]) {
    if frame.len() < 42 {
        return;
    }
    if frame[12] != 0x08 || frame[13] != 0x06 {
        return; // not ARP
    }
    let a = &frame[14..];
    if a[6] != 0x00 || a[7] != 0x02 {
        return; // not a reply
    }
    dev.gateway_mac.copy_from_slice(&a[8..14]);
    dev.have_gateway_mac = true;
    let m = dev.gateway_mac;
    kprintln!(
        "[NET] Learned gateway MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        m[0],
        m[1],
        m[2],
        m[3],
        m[4],
        m[5]
    );
}

const ICMP_PING_PAYLOAD: usize = 56;

fn send_ping_locked(dev: &mut NetDevice, seq: u16) -> i64 {
    if !dev.have_gateway_mac {
        Uart.errorln("[NET] ping: no gateway MAC (run ARP first)");
        return -1;
    }
    let mut f = [0u8; 14 + 20 + 8 + ICMP_PING_PAYLOAD];

    // Ethernet.
    f[0..6].copy_from_slice(&dev.gateway_mac);
    f[6..12].copy_from_slice(&dev.mac);
    f[12] = 0x08;
    f[13] = 0x00;

    // IPv4.
    {
        let ip = &mut f[14..34];
        ip[0] = 0x45;
        let total = (20 + 8 + ICMP_PING_PAYLOAD) as u16;
        ip[2] = (total >> 8) as u8;
        ip[3] = total as u8;
        ip[8] = 64; // TTL
        ip[9] = 1; // ICMP
        ip[12..16].copy_from_slice(&dev.my_ip);
        ip[16..20].copy_from_slice(&dev.gateway_ip);
    }
    let ipcsum = inet_csum(&f[14..34]);
    f[24] = (ipcsum >> 8) as u8;
    f[25] = ipcsum as u8;

    // ICMP echo request.
    {
        let icmp = &mut f[34..];
        icmp[0] = 8; // echo request
        icmp[5] = 42; // identifier
        icmp[6] = (seq >> 8) as u8;
        icmp[7] = seq as u8;
        for i in 0..ICMP_PING_PAYLOAD {
            icmp[8 + i] = b'a' + (i % 26) as u8;
        }
    }
    let iccsum = inet_csum(&f[34..]);
    f[36] = (iccsum >> 8) as u8;
    f[37] = iccsum as u8;

    kprintln!("[NET] Sending ICMP echo request seq={} to 10.0.2.2", seq);
    tx_locked(dev, &f)
}

fn send_arp_probe_locked(dev: &mut NetDevice) -> i64 {
    if !dev.have_mac {
        Uart.errorln("[NET] arp_probe: no MAC negotiated");
        return -1;
    }
    let mut f = [0u8; 60];
    for i in 0..6 {
        f[i] = 0xFF; // broadcast dst
    }
    f[6..12].copy_from_slice(&dev.mac);
    f[12] = 0x08;
    f[13] = 0x06; // ARP
    let a = &mut f[14..];
    a[0] = 0x00;
    a[1] = 0x01; // HTYPE Ethernet
    a[2] = 0x08;
    a[3] = 0x00; // PTYPE IPv4
    a[4] = 6;
    a[5] = 4; // HLEN / PLEN
    a[6] = 0x00;
    a[7] = 0x01; // request
    a[8..14].copy_from_slice(&dev.mac);
    a[14..18].copy_from_slice(&dev.my_ip);
    a[24..28].copy_from_slice(&dev.gateway_ip);

    kprintln!("[NET] Sending ARP probe for 10.0.2.2 (slirp gateway)");
    tx_locked(dev, &f)
}

/// EL0-callable ICMP echo: send one ping to the gateway and poll for the reply.
/// Returns the reply TTL on success, or -1.
pub fn send_ping(seq: u16) -> i64 {
    let mut g = NET.lock();
    let dev = match g.as_mut() {
        Some(d) if d.ready => d,
        _ => return -1,
    };
    if send_ping_locked(dev, seq) <= 0 {
        return -1;
    }
    let mut buf = [0u8; 256];
    let mut spins = 0u32;
    while spins < 2_000_000 {
        spins += 1;
        let n = rx_poll_locked(dev, &mut buf);
        if n < (14 + 20 + 8) {
            continue;
        }
        if buf[12] != 0x08 || buf[13] != 0x00 {
            continue; // not IPv4
        }
        let ip = &buf[14..];
        let icmp = &buf[34..];
        if ip[9] != 1 || icmp[0] != 0 {
            continue; // not ICMP echo reply
        }
        let reply_seq = ((icmp[6] as u16) << 8) | icmp[7] as u16;
        if reply_seq != seq {
            continue;
        }
        return ip[8] as i64; // TTL
    }
    -1
}

/// Render /proc/netinfo into `out`; returns bytes written.
pub fn get_info(out: &mut [u8]) -> usize {
    let g = NET.lock();
    let mut w = FmtBuf::new(out);
    match g.as_ref() {
        Some(d) => {
            let m = d.mac;
            let _ = write!(
                w,
                "mac:        {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\n",
                m[0], m[1], m[2], m[3], m[4], m[5]
            );
            let _ = write!(
                w,
                "link:       {}\n",
                if d.link_status & S_LINK_UP != 0 {
                    "UP"
                } else {
                    "DOWN"
                }
            );
            let ip = d.my_ip;
            let nm = d.subnet_mask;
            let gw = d.gateway_ip;
            let ds = d.dhcp_server;
            let _ = write!(
                w,
                "ip:         {}.{}.{}.{}\nnetmask:    {}.{}.{}.{}\ngateway:    {}.{}.{}.{}\ndhcp:       {}\ndhcp_srv:   {}.{}.{}.{}\nlease:      {} s\n",
                ip[0], ip[1], ip[2], ip[3],
                nm[0], nm[1], nm[2], nm[3],
                gw[0], gw[1], gw[2], gw[3],
                if d.dhcp_acquired { "yes" } else { "no" },
                ds[0], ds[1], ds[2], ds[3],
                d.lease_secs
            );
            if d.have_gateway_mac {
                let g2 = d.gateway_mac;
                let _ = write!(
                    w,
                    "gw_mac:     {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\n",
                    g2[0], g2[1], g2[2], g2[3], g2[4], g2[5]
                );
            } else {
                let _ = write!(w, "gw_mac:     (unknown)\n");
            }
            let _ = write!(
                w,
                "rx_packets: {}\ntx_packets: {}\n",
                d.rx_packets, d.tx_packets
            );
        }
        None => {
            let _ = write!(w, "net: not available\n");
        }
    }
    w.len()
}

// --- DHCP client (RFC 2131) -------------------------------------------------

const DHCP_DISCOVER: u8 = 1;
const DHCP_OFFER: u8 = 2;
const DHCP_REQUEST: u8 = 3;
const DHCP_ACK: u8 = 5;
const DHCP_MAGIC: [u8; 4] = [0x63, 0x82, 0x53, 0x63];
const OPT_SUBNET: u8 = 1;
const OPT_ROUTER: u8 = 3;
const OPT_REQ_IP: u8 = 50;
const OPT_LEASE: u8 = 51;
const OPT_MSGTYPE: u8 = 53;
const OPT_SERVER_ID: u8 = 54;
const OPT_PARAM_REQ: u8 = 55;
const OPT_END: u8 = 255;

/// Build an Eth/IPv4/UDP frame around `payload` into `frame`; returns total len.
fn udp_build(
    dev: &NetDevice,
    frame: &mut [u8],
    dst_mac: &[u8; 6],
    src_ip: &[u8; 4],
    dst_ip: &[u8; 4],
    src_port: u16,
    dst_port: u16,
    payload: &[u8],
) -> usize {
    let udp_len = 8 + payload.len();
    let ip_len = 20 + udp_len;
    let total = 14 + ip_len;

    frame[0..6].copy_from_slice(dst_mac);
    frame[6..12].copy_from_slice(&dev.mac);
    frame[12] = 0x08;
    frame[13] = 0x00;

    {
        let ip = &mut frame[14..34];
        ip[0] = 0x45;
        ip[1] = 0;
        ip[2] = (ip_len >> 8) as u8;
        ip[3] = ip_len as u8;
        ip[4] = 0;
        ip[5] = 0; // id
        ip[6] = 0;
        ip[7] = 0; // flags + frag
        ip[8] = 64;
        ip[9] = 17; // UDP
        ip[10] = 0; // checksum field MUST be zero before computing inet_csum;
        ip[11] = 0; // `frame` is reused across DISCOVER/REQUEST without zeroing.
        ip[12..16].copy_from_slice(src_ip);
        ip[16..20].copy_from_slice(dst_ip);
    }
    let ipcsum = inet_csum(&frame[14..34]);
    frame[24] = (ipcsum >> 8) as u8;
    frame[25] = ipcsum as u8;

    {
        let udp = &mut frame[34..];
        udp[0] = (src_port >> 8) as u8;
        udp[1] = src_port as u8;
        udp[2] = (dst_port >> 8) as u8;
        udp[3] = dst_port as u8;
        udp[4] = (udp_len >> 8) as u8;
        udp[5] = udp_len as u8;
        // checksum 0 (legal for IPv4)
        udp[8..8 + payload.len()].copy_from_slice(payload);
    }
    total
}

/// Build a DHCP DISCOVER/REQUEST into `bootp`; returns bytes written.
fn dhcp_build(
    bootp: &mut [u8],
    msg_type: u8,
    client_mac: &[u8; 6],
    xid: u32,
    requested_ip: Option<&[u8; 4]>,
    server_id: Option<&[u8; 4]>,
) -> usize {
    for b in bootp[..240].iter_mut() {
        *b = 0;
    }
    bootp[0] = 1; // BOOTREQUEST
    bootp[1] = 1; // Ethernet
    bootp[2] = 6; // hlen
    bootp[4] = (xid >> 24) as u8;
    bootp[5] = (xid >> 16) as u8;
    bootp[6] = (xid >> 8) as u8;
    bootp[7] = xid as u8;
    bootp[28..34].copy_from_slice(client_mac);
    bootp[236..240].copy_from_slice(&DHCP_MAGIC);

    let mut p = 240;
    bootp[p] = OPT_MSGTYPE;
    bootp[p + 1] = 1;
    bootp[p + 2] = msg_type;
    p += 3;
    bootp[p] = OPT_PARAM_REQ;
    bootp[p + 1] = 4;
    p += 2;
    bootp[p] = OPT_SUBNET;
    bootp[p + 1] = OPT_ROUTER;
    bootp[p + 2] = OPT_LEASE;
    bootp[p + 3] = OPT_SERVER_ID;
    p += 4;
    if let Some(ip) = requested_ip {
        bootp[p] = OPT_REQ_IP;
        bootp[p + 1] = 4;
        p += 2;
        bootp[p..p + 4].copy_from_slice(ip);
        p += 4;
    }
    if let Some(sid) = server_id {
        bootp[p] = OPT_SERVER_ID;
        bootp[p + 1] = 4;
        p += 2;
        bootp[p..p + 4].copy_from_slice(sid);
        p += 4;
    }
    bootp[p] = OPT_END;
    p += 1;
    p
}

fn dhcp_find_option(opts: &[u8], want: u8) -> Option<&[u8]> {
    let mut p = 0;
    while p < opts.len() {
        let code = opts[p];
        if code == OPT_END {
            return None;
        }
        if code == 0 {
            p += 1;
            continue;
        }
        if p + 1 >= opts.len() {
            return None;
        }
        let len = opts[p + 1] as usize;
        if p + 2 + len > opts.len() {
            return None;
        }
        if code == want {
            return Some(&opts[p + 2..p + 2 + len]);
        }
        p += 2 + len;
    }
    None
}

struct DhcpReply {
    yiaddr: [u8; 4],
    server: [u8; 4],
    mask: [u8; 4],
    router: [u8; 4],
    lease: u32,
}

fn dhcp_parse(frame: &[u8], expect_xid: u32, expect_msg: u8) -> Option<DhcpReply> {
    if frame.len() < 14 + 20 + 8 + 240 {
        return None;
    }
    if frame[12] != 0x08 || frame[13] != 0x00 {
        return None;
    }
    let ip = &frame[14..];
    if (ip[0] & 0xF0) != 0x40 || ip[9] != 17 {
        return None;
    }
    let ihl = ((ip[0] & 0x0F) * 4) as usize;
    if ihl != 20 {
        return None;
    }
    let udp = &frame[14 + ihl..];
    let src_port = ((udp[0] as u16) << 8) | udp[1] as u16;
    let dst_port = ((udp[2] as u16) << 8) | udp[3] as u16;
    let udp_len = ((udp[4] as u16) << 8) | udp[5] as u16;
    if src_port != 67 || dst_port != 68 || (udp_len as usize) < 8 + 240 {
        return None;
    }
    let bootp = &udp[8..];
    if bootp[0] != 2 {
        return None;
    }
    let xid = ((bootp[4] as u32) << 24)
        | ((bootp[5] as u32) << 16)
        | ((bootp[6] as u32) << 8)
        | bootp[7] as u32;
    if xid != expect_xid {
        return None;
    }
    if bootp[236..240] != DHCP_MAGIC {
        return None;
    }
    let opts_len = udp_len as usize - 8 - 240;
    let opts = &bootp[240..240 + opts_len];

    let mt = dhcp_find_option(opts, OPT_MSGTYPE)?;
    if mt.len() != 1 || mt[0] != expect_msg {
        return None;
    }
    let mut r = DhcpReply {
        yiaddr: [0; 4],
        server: [0; 4],
        mask: [255, 255, 255, 0],
        router: [0; 4],
        lease: 0,
    };
    r.yiaddr.copy_from_slice(&bootp[16..20]);
    if let Some(s) = dhcp_find_option(opts, OPT_SERVER_ID) {
        if s.len() == 4 {
            r.server.copy_from_slice(s);
        }
    }
    if let Some(m) = dhcp_find_option(opts, OPT_SUBNET) {
        if m.len() == 4 {
            r.mask.copy_from_slice(m);
        }
    }
    if let Some(rt) = dhcp_find_option(opts, OPT_ROUTER) {
        if rt.len() >= 4 {
            r.router.copy_from_slice(&rt[..4]);
        }
    }
    if let Some(ls) = dhcp_find_option(opts, OPT_LEASE) {
        if ls.len() == 4 {
            r.lease = ((ls[0] as u32) << 24)
                | ((ls[1] as u32) << 16)
                | ((ls[2] as u32) << 8)
                | ls[3] as u32;
        }
    }
    Some(r)
}

fn dhcp_acquire_locked(dev: &mut NetDevice) -> bool {
    if !dev.have_mac {
        Uart.errorln("[DHCP] no MAC; can't run");
        return false;
    }
    kprintln!("[DHCP] Starting acquire...");

    let bcast_mac = [0xFFu8; 6];
    let any_ip = [0u8; 4];
    let bcast_ip = [255u8; 4];
    let xid = 0xFE22_1001u32;

    let mut frame = [0u8; 14 + 20 + 8 + 240 + 64];
    let mut bootp = [0u8; 240 + 64];
    let mut rx = [0u8; 600];

    // DISCOVER.
    let blen = dhcp_build(&mut bootp, DHCP_DISCOVER, &dev.mac, xid, None, None);
    let flen = udp_build(
        dev,
        &mut frame,
        &bcast_mac,
        &any_ip,
        &bcast_ip,
        68,
        67,
        &bootp[..blen],
    );
    if tx_locked(dev, &frame[..flen]) <= 0 {
        Uart.errorln("[DHCP] DISCOVER TX failed");
        return false;
    }
    kprintln!("[DHCP] DISCOVER sent");

    // Wait for OFFER.
    let offer = {
        let mut found = None;
        let mut s = 0u32;
        while s < 5_000_000 && found.is_none() {
            s += 1;
            let n = rx_poll_locked(dev, &mut rx);
            if n > 0 {
                found = dhcp_parse(&rx[..n as usize], xid, DHCP_OFFER);
            }
        }
        found
    };
    let offer = match offer {
        Some(o) => o,
        None => {
            Uart.errorln("[DHCP] no OFFER received");
            return false;
        }
    };
    kprintln!(
        "[DHCP] OFFER: {}.{}.{}.{} (server={}.{}.{}.{}, lease={}s)",
        offer.yiaddr[0],
        offer.yiaddr[1],
        offer.yiaddr[2],
        offer.yiaddr[3],
        offer.server[0],
        offer.server[1],
        offer.server[2],
        offer.server[3],
        offer.lease
    );

    // REQUEST.
    let blen = dhcp_build(
        &mut bootp,
        DHCP_REQUEST,
        &dev.mac,
        xid,
        Some(&offer.yiaddr),
        Some(&offer.server),
    );
    let flen = udp_build(
        dev,
        &mut frame,
        &bcast_mac,
        &any_ip,
        &bcast_ip,
        68,
        67,
        &bootp[..blen],
    );
    if tx_locked(dev, &frame[..flen]) <= 0 {
        Uart.errorln("[DHCP] REQUEST TX failed");
        return false;
    }
    kprintln!("[DHCP] REQUEST sent");

    // Wait for ACK.
    let ack = {
        let mut found = None;
        let mut s = 0u32;
        while s < 5_000_000 && found.is_none() {
            s += 1;
            let n = rx_poll_locked(dev, &mut rx);
            if n > 0 {
                found = dhcp_parse(&rx[..n as usize], xid, DHCP_ACK);
            }
        }
        found
    };
    let ack = match ack {
        Some(a) => a,
        None => {
            Uart.errorln("[DHCP] no ACK received");
            return false;
        }
    };

    dev.my_ip = ack.yiaddr;
    dev.subnet_mask = ack.mask;
    dev.gateway_ip = ack.router;
    dev.dhcp_server = ack.server;
    dev.lease_secs = ack.lease;
    dev.dhcp_acquired = true;
    let ip = dev.my_ip;
    kprintln!(
        "[DHCP] Lease ACK ip={}.{}.{}.{} lease={}s",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        dev.lease_secs
    );
    true
}

// --- Init -------------------------------------------------------------------

/// Discover and initialize virtio-net, run DHCP, and do an ARP+ping smoke test.
pub fn init() {
    kprintln!("[NET] Initializing Device");
    let mut pdev = match pci::find_device(VIRTIO_NET_VENDOR_ID, VIRTIO_NET_DEVICE_ID) {
        Some(d) => d,
        None => {
            Uart.errorln("[NET] Device not found");
            return;
        }
    };
    kprintln!("[NET] Device found");

    if pci::get_header_type(&pdev) & 0x7F != PCI_ENDPOINT_DEV_TYPE {
        Uart.errorln("[NET] Unexpected header type");
        return;
    }
    pci::assign_bars(&mut pdev);
    pci::enable_device(&pdev);
    let mut caps = VirtioPciCaps::default();
    parse_capabilities(&pdev, &mut caps);
    let base = caps.common_cfg;
    if base == 0 {
        Uart.errorln("[NET] No common config capability");
        return;
    }

    // Negotiate VERSION_1 (required) + MAC + STATUS (in the low word). We pass
    // the low-word features we want via a custom handshake here rather than the
    // shared helper (which only takes extra HIGH bits).
    use crate::klib::mmio;
    mmio::write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    dsb_sy();
    while mmio::read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET {}
    let mut status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(
        base + VIRTIO_COMMON_STATUS,
        status | VIRTIO_STATUS_ACKNOWLEDGE,
    );
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
    dsb_sy();

    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 0);
    dsb_sy();
    let feat_lo = mmio::read32(base + VIRTIO_COMMON_DF);
    mmio::write32(base + VIRTIO_COMMON_DFSELECT, 1);
    dsb_sy();
    let feat_hi = mmio::read32(base + VIRTIO_COMMON_DF);
    kprintln!("[NET] device features: lo={:#x} hi={:#x}", feat_lo, feat_hi);
    if feat_hi & 0x01 == 0 {
        Uart.errorln("[NET] Device does not advertise VIRTIO_F_VERSION_1");
        return;
    }
    let guest_lo = feat_lo & (F_MAC | F_STATUS);
    let guest_hi = feat_hi & 0x01;
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 0);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_lo);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GFSELECT, 1);
    dsb_sy();
    mmio::write32(base + VIRTIO_COMMON_GF, guest_hi);
    dsb_sy();
    kprintln!(
        "[NET] accepted features: lo={:#x} hi={:#x}",
        guest_lo,
        guest_hi
    );

    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    mmio::write8(
        base + VIRTIO_COMMON_STATUS,
        status | VIRTIO_STATUS_FEATURES_OK,
    );
    dsb_sy();
    status = mmio::read8(base + VIRTIO_COMMON_STATUS);
    if status & VIRTIO_STATUS_FEATURES_OK == 0 {
        Uart.errorln("[NET] FEATURES_OK failed");
        return;
    }
    kprintln!("[NET] FEATURES_OK (status={:#x})", status);

    // Set up RX (0) + TX (1) queues over the static rings.
    // SAFETY: ring statics owned by this driver.
    let mut rx_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(RX_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(RX_AVAIL.0),
            used: core::ptr::addr_of_mut!(RX_USED.0),
        }
    };
    if !rx_vq.setup(base, QUEUE_RX, &caps) {
        Uart.errorln("[NET] RX virtqueue setup failed");
        return;
    }
    let mut tx_vq = unsafe {
        Virtqueue {
            size: 0,
            free_head: 0,
            last_used: 0,
            notify_addr: 0,
            desc: core::ptr::addr_of_mut!(TX_DESC.0) as *mut VirtqDesc,
            avail: core::ptr::addr_of_mut!(TX_AVAIL.0),
            used: core::ptr::addr_of_mut!(TX_USED.0),
        }
    };
    if !tx_vq.setup(base, QUEUE_TX, &caps) {
        Uart.errorln("[NET] TX virtqueue setup failed");
        return;
    }

    if !set_driver_ok(base) {
        return;
    }
    kprintln!("[NET] DRIVER_OK set");

    let mut dev = NetDevice {
        rx_vq,
        tx_vq,
        mac: [0; 6],
        have_mac: false,
        link_status: 0,
        rx_desc_to_buf: [-1; VIRTQ_MAX_SIZE],
        rx_initialized: false,
        rx_packets: 0,
        tx_packets: 0,
        gateway_mac: [0; 6],
        have_gateway_mac: false,
        my_ip: [10, 0, 2, 15],
        subnet_mask: [255, 255, 255, 0],
        gateway_ip: [10, 0, 2, 2],
        dhcp_server: [0; 4],
        lease_secs: 0,
        dhcp_acquired: false,
        ready: true,
    };

    // Prime RX before any TX so a slirp ARP reply has buffers waiting.
    rx_init(&mut dev);

    // MAC + link from device config.
    let dcfg = caps.device_cfg;
    if guest_lo & F_MAC != 0 {
        for i in 0..6 {
            dev.mac[i] = mmio::read8(dcfg + CFG_MAC + i);
        }
        dev.have_mac = true;
        let m = dev.mac;
        kprintln!(
            "[NET] MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            m[0],
            m[1],
            m[2],
            m[3],
            m[4],
            m[5]
        );
    } else {
        kprintln!("[NET] Device did not advertise VIRTIO_NET_F_MAC");
    }
    if guest_lo & F_STATUS != 0 {
        dev.link_status = mmio::read16(dcfg + CFG_STATUS);
        kprintln!(
            "[NET] Link: {} (status={:#x})",
            if dev.link_status & S_LINK_UP != 0 {
                "UP"
            } else {
                "DOWN"
            },
            dev.link_status
        );
    }

    // DHCP, then ARP probe + ICMP echo smoke test.
    dhcp_acquire_locked(&mut dev);

    if send_arp_probe_locked(&mut dev) > 0 {
        kprintln!("[NET] ARP probe TX accepted by device");
    }
    let mut rx_buf = [0u8; 256];
    let mut spins = 0u32;
    while spins < 1_000_000 {
        spins += 1;
        let n = rx_poll_locked(&mut dev, &mut rx_buf);
        if n > 0 {
            parse_arp_reply(&mut dev, &rx_buf[..n as usize]);
            break;
        }
    }
    if dev.have_gateway_mac && send_ping_locked(&mut dev, 1) > 0 {
        let mut spins = 0u32;
        while spins < 2_000_000 {
            spins += 1;
            let n = rx_poll_locked(&mut dev, &mut rx_buf);
            if n >= (14 + 20 + 8) && rx_buf[12] == 0x08 && rx_buf[13] == 0x00 {
                let ip = &rx_buf[14..];
                let icmp = &rx_buf[34..];
                if ip[9] == 1 && icmp[0] == 0 {
                    kprintln!(
                        "[NET] PING reply from {}.{}.{}.{} ttl={} seq={} \\o/",
                        ip[12],
                        ip[13],
                        ip[14],
                        ip[15],
                        ip[8],
                        ((icmp[6] as u32) << 8) | icmp[7] as u32
                    );
                    break;
                }
            }
        }
    }

    *NET.lock() = Some(dev);
}
