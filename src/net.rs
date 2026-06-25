//! Layer-2/3 networking: ARP, IPv4 + ICMP echo, and a minimal DHCP client.
//!
//! Port of the protocol portion of `src/pci/virtio/net/net.c`. Uses the
//! virtio-net driver (`crate::virtio::net`) for raw frame TX/RX.

use crate::kprintln;
use crate::sync::Racy;
use crate::uart;
use crate::exception::timer;
use crate::virtio::net;

pub struct NetState {
    pub my_ip: [u8; 4],
    pub subnet_mask: [u8; 4],
    pub gateway_ip: [u8; 4],
    pub dhcp_server: [u8; 4],
    pub lease_secs: u32,
    pub dhcp_acquired: bool,
    pub gateway_mac: [u8; 6],
    pub have_gateway_mac: bool,
    pub arp_cache: [([u8; 4], [u8; 6], bool); 8],
}

static NET: Racy<NetState> = Racy::new(NetState {
    my_ip: [10, 0, 2, 15],
    subnet_mask: [255, 255, 255, 0],
    gateway_ip: [10, 0, 2, 2],
    dhcp_server: [0, 0, 0, 0],
    lease_secs: 0,
    dhcp_acquired: false,
    gateway_mac: [0; 6],
    have_gateway_mac: false,
    arp_cache: [([0;4],[0;6],false); 8],
});

fn state() -> &'static mut NetState {
    unsafe { NET.get() }
}

/// Sleep until the next interrupt (net RX SPI or timer tick) instead of
/// busy-spinning. The kernel runs at EL1 with IRQs unmasked.
#[inline(always)]
fn idle_wait() {
    unsafe { core::arch::asm!("wfi") };
}

fn my_mac() -> [u8; 6] {
    net::dev().mac
}

/// RFC 1071 internet checksum (network-order result).
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

/// Broadcast an ARP request for the gateway IP.
pub fn send_arp_probe() -> i32 {
    let target = state().gateway_ip;
    send_arp_for(&target)
}

/// Broadcast an ARP request for an arbitrary target IPv4 address.
pub fn send_arp_for(target_ip: &[u8; 4]) -> i32 {
    let nd = net::dev();
    if !nd.have_mac {
        uart::errorln("[NET] arp: no MAC");
        return -1;
    }
    let st = state();
    let mac = nd.mac;
    let mut f = [0u8; 60];
    for i in 0..6 {
        f[i] = 0xFF; // dst broadcast
        f[6 + i] = mac[i]; // src
    }
    f[12] = 0x08;
    f[13] = 0x06; // ARP
    let a = &mut f[14..];
    a[0] = 0x00; a[1] = 0x01; // HTYPE ethernet
    a[2] = 0x08; a[3] = 0x00; // PTYPE ipv4
    a[4] = 6; a[5] = 4;
    a[6] = 0x00; a[7] = 0x01; // request
    for i in 0..6 {
        a[8 + i] = mac[i];
    }
    for i in 0..4 {
        a[14 + i] = st.my_ip[i];
    }
    for i in 0..4 {
        a[24 + i] = target_ip[i];
    }
    net::tx(&f)
}

/// Parse an ARP reply and cache the sender's MAC as the gateway MAC.
pub fn parse_arp_reply(frame: &[u8]) {
    if frame.len() < 42 {
        return;
    }
    if frame[12] != 0x08 || frame[13] != 0x06 {
        return;
    }
    let a = &frame[14..];
    if a[6] != 0x00 || a[7] != 0x02 {
        return; // not a reply
    }
    let st = state();
    // Sender protocol address (SPA) at a[14..18], sender HW (SHA) at a[8..14].
    let mut sip = [0u8; 4];
    sip.copy_from_slice(&a[14..18]);
    let mut smac = [0u8; 6];
    smac.copy_from_slice(&a[8..14]);
    // Insert/update the ARP cache.
    let mut slot = None;
    for (i, e) in st.arp_cache.iter().enumerate() {
        if e.2 && e.0 == sip { slot = Some(i); break; }
    }
    let slot = slot.or_else(|| st.arp_cache.iter().position(|e| !e.2)).unwrap_or(0);
    st.arp_cache[slot] = (sip, smac, true);
    // Track gateway MAC specifically.
    if sip == st.gateway_ip {
        st.gateway_mac = smac;
        st.have_gateway_mac = true;
    }
    kprintln!(
        "[NET] Learned gateway MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        st.gateway_mac[0], st.gateway_mac[1], st.gateway_mac[2],
        st.gateway_mac[3], st.gateway_mac[4], st.gateway_mac[5]
    );
}

const ICMP_PING_PAYLOAD: usize = 56;

/// Send an ICMP echo request to the gateway (requires learned gateway MAC).
pub fn send_ping(seq: u16) -> i32 {
    let st = state();
    if !st.have_gateway_mac {
        uart::errorln("[NET] ping: no gateway MAC");
        return -1;
    }
    let mac = my_mac();
    let mut f = [0u8; 14 + 20 + 8 + ICMP_PING_PAYLOAD];
    for i in 0..6 {
        f[i] = st.gateway_mac[i];
        f[6 + i] = mac[i];
    }
    f[12] = 0x08;
    f[13] = 0x00; // IPv4
    // IPv4 header
    {
        let ip = &mut f[14..34];
        ip[0] = 0x45;
        let total = (20 + 8 + ICMP_PING_PAYLOAD) as u16;
        ip[2] = (total >> 8) as u8;
        ip[3] = total as u8;
        ip[8] = 64; // TTL
        ip[9] = 1; // ICMP
        for i in 0..4 {
            ip[12 + i] = st.my_ip[i];
            ip[16 + i] = st.gateway_ip[i];
        }
        let csum = inet_csum(&ip[..20]);
        ip[10] = (csum >> 8) as u8;
        ip[11] = csum as u8;
    }
    // ICMP echo request
    {
        let icmp = &mut f[34..];
        icmp[0] = 8; // echo request
        icmp[5] = 42; // identifier
        icmp[6] = (seq >> 8) as u8;
        icmp[7] = seq as u8;
        for i in 0..ICMP_PING_PAYLOAD {
            icmp[8 + i] = b'a' + (i % 26) as u8;
        }
        let len = 8 + ICMP_PING_PAYLOAD;
        let csum = inet_csum(&icmp[..len]);
        icmp[2] = (csum >> 8) as u8;
        icmp[3] = csum as u8;
    }
    kprintln!("[NET] Sending ICMP echo seq={}", seq);
    net::tx(&f)
}

// --------------------------- DHCP client ---------------------------

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

/// Build an Ethernet/IPv4/UDP frame around `payload`. Returns total length.
fn udp_build(
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
    let mac = my_mac();
    for i in 0..6 {
        frame[i] = dst_mac[i];
        frame[6 + i] = mac[i];
    }
    frame[12] = 0x08;
    frame[13] = 0x00;
    {
        let ip = &mut frame[14..34];
        ip[0] = 0x45;
        ip[2] = (ip_len >> 8) as u8;
        ip[3] = ip_len as u8;
        ip[8] = 64;
        ip[9] = 17; // UDP
        for i in 0..4 {
            ip[12 + i] = src_ip[i];
            ip[16 + i] = dst_ip[i];
        }
        let csum = inet_csum(&ip[..20]);
        ip[10] = (csum >> 8) as u8;
        ip[11] = csum as u8;
    }
    {
        let udp = &mut frame[34..42];
        udp[0] = (src_port >> 8) as u8;
        udp[1] = src_port as u8;
        udp[2] = (dst_port >> 8) as u8;
        udp[3] = dst_port as u8;
        udp[4] = (udp_len >> 8) as u8;
        udp[5] = udp_len as u8;
    }
    frame[42..42 + payload.len()].copy_from_slice(payload);
    total
}

fn dhcp_build(
    bootp: &mut [u8],
    msg_type: u8,
    client_mac: &[u8; 6],
    xid: u32,
    requested_ip: Option<&[u8; 4]>,
    server_id: Option<&[u8; 4]>,
) -> usize {
    for x in bootp[..240].iter_mut() {
        *x = 0;
    }
    bootp[0] = 1; // BOOTREQUEST
    bootp[1] = 1;
    bootp[2] = 6;
    bootp[4] = (xid >> 24) as u8;
    bootp[5] = (xid >> 16) as u8;
    bootp[6] = (xid >> 8) as u8;
    bootp[7] = xid as u8;
    for i in 0..6 {
        bootp[28 + i] = client_mac[i];
    }
    bootp[236..240].copy_from_slice(&DHCP_MAGIC);
    let mut p = 240;
    bootp[p] = OPT_MSGTYPE; bootp[p + 1] = 1; bootp[p + 2] = msg_type; p += 3;
    bootp[p] = OPT_PARAM_REQ; bootp[p + 1] = 4; p += 2;
    bootp[p] = OPT_SUBNET; bootp[p + 1] = OPT_ROUTER; bootp[p + 2] = OPT_LEASE; bootp[p + 3] = OPT_SERVER_ID; p += 4;
    if let Some(ip) = requested_ip {
        bootp[p] = OPT_REQ_IP; bootp[p + 1] = 4; p += 2;
        bootp[p..p + 4].copy_from_slice(ip); p += 4;
    }
    if let Some(sid) = server_id {
        bootp[p] = OPT_SERVER_ID; bootp[p + 1] = 4; p += 2;
        bootp[p..p + 4].copy_from_slice(sid); p += 4;
    }
    bootp[p] = OPT_END; p += 1;
    p
}

fn dhcp_find_option<'a>(opts: &'a [u8], want: u8) -> Option<&'a [u8]> {
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
    if ip[0] & 0xF0 != 0x40 || ip[9] != 17 {
        return None;
    }
    let ihl = (ip[0] & 0x0F) as usize * 4;
    if ihl != 20 {
        return None;
    }
    let udp = &ip[ihl..];
    let src_port = ((udp[0] as u16) << 8) | udp[1] as u16;
    let dst_port = ((udp[2] as u16) << 8) | udp[3] as u16;
    let udp_len = (((udp[4] as u16) << 8) | udp[5] as u16) as usize;
    if src_port != 67 || dst_port != 68 || udp_len < 8 + 240 {
        return None;
    }
    let bootp = &udp[8..];
    if bootp[0] != 2 {
        return None;
    }
    let xid = ((bootp[4] as u32) << 24) | ((bootp[5] as u32) << 16) | ((bootp[6] as u32) << 8) | bootp[7] as u32;
    if xid != expect_xid {
        return None;
    }
    if bootp[236..240] != DHCP_MAGIC {
        return None;
    }
    let opts_len = udp_len - 8 - 240;
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
    if let Some(sid) = dhcp_find_option(opts, OPT_SERVER_ID) {
        if sid.len() == 4 {
            r.server.copy_from_slice(sid);
        }
    }
    if let Some(sm) = dhcp_find_option(opts, OPT_SUBNET) {
        if sm.len() == 4 {
            r.mask.copy_from_slice(sm);
        }
    }
    if let Some(rt) = dhcp_find_option(opts, OPT_ROUTER) {
        if rt.len() >= 4 {
            r.router.copy_from_slice(&rt[..4]);
        }
    }
    if let Some(ls) = dhcp_find_option(opts, OPT_LEASE) {
        if ls.len() == 4 {
            r.lease = ((ls[0] as u32) << 24) | ((ls[1] as u32) << 16) | ((ls[2] as u32) << 8) | ls[3] as u32;
        }
    }
    Some(r)
}

/// Run a synchronous DHCP DISCOVER->OFFER->REQUEST->ACK exchange.
pub fn dhcp_acquire() -> bool {
    let nd = net::dev();
    if !nd.have_mac {
        uart::errorln("[DHCP] no MAC");
        return false;
    }
    uart::println("[DHCP] Starting acquire...");
    let mac = nd.mac;
    let bcast_mac = [0xFFu8; 6];
    let any_ip = [0u8; 4];
    let bcast_ip = [255u8; 4];
    let xid = 0xFE22_1001u32;

    let mut bootp = [0u8; 240 + 64];
    let mut frame = [0u8; 14 + 20 + 8 + 240 + 64];
    let mut rx = [0u8; 600];

    // DISCOVER
    let blen = dhcp_build(&mut bootp, DHCP_DISCOVER, &mac, xid, None, None);
    let flen = udp_build(&mut frame, &bcast_mac, &any_ip, &bcast_ip, 68, 67, &bootp[..blen]);
    if net::tx(&frame[..flen]) <= 0 {
        uart::errorln("[DHCP] DISCOVER TX failed");
        return false;
    }
    uart::println("[DHCP] DISCOVER sent");

    let offer = poll_dhcp(&mut rx, xid, DHCP_OFFER, 300);
    let offer = match offer {
        Some(o) => o,
        None => {
            uart::errorln("[DHCP] no OFFER");
            return false;
        }
    };
    kprintln!(
        "[DHCP] OFFER {}.{}.{}.{}",
        offer.yiaddr[0], offer.yiaddr[1], offer.yiaddr[2], offer.yiaddr[3]
    );

    // REQUEST
    let blen = dhcp_build(&mut bootp, DHCP_REQUEST, &mac, xid, Some(&offer.yiaddr), Some(&offer.server));
    let flen = udp_build(&mut frame, &bcast_mac, &any_ip, &bcast_ip, 68, 67, &bootp[..blen]);
    if net::tx(&frame[..flen]) <= 0 {
        uart::errorln("[DHCP] REQUEST TX failed");
        return false;
    }
    uart::println("[DHCP] REQUEST sent");

    let ack = poll_dhcp(&mut rx, xid, DHCP_ACK, 300);
    let ack = match ack {
        Some(a) => a,
        None => {
            uart::errorln("[DHCP] no ACK");
            return false;
        }
    };
    let st = state();
    st.my_ip = ack.yiaddr;
    st.subnet_mask = ack.mask;
    st.gateway_ip = ack.router;
    st.dhcp_server = ack.server;
    st.lease_secs = ack.lease;
    st.dhcp_acquired = true;
    kprintln!(
        "[DHCP] ACK ip={}.{}.{}.{} gw={}.{}.{}.{} lease={}s",
        st.my_ip[0], st.my_ip[1], st.my_ip[2], st.my_ip[3],
        st.gateway_ip[0], st.gateway_ip[1], st.gateway_ip[2], st.gateway_ip[3],
        st.lease_secs
    );
    true
}

fn poll_dhcp(rx: &mut [u8], xid: u32, msg: u8, tick_budget: u64) -> Option<DhcpReply> {
    let deadline = timer::get_ticks() + tick_budget;
    while timer::get_ticks() < deadline {
        let n = net::rx_poll(rx);
        if n > 0 {
            if let Some(r) = dhcp_parse(&rx[..n as usize], xid, msg) {
                return Some(r);
            }
        }
        idle_wait();
    }
    None
}

/// Look up a cached MAC for an IPv4 address.
pub fn arp_lookup(ip: &[u8; 4]) -> Option<[u8; 6]> {
    for e in state().arp_cache.iter() {
        if e.2 && &e.0 == ip {
            return Some(e.1);
        }
    }
    None
}

/// Render the ARP cache.
pub fn render_arp() -> alloc::string::String {
    use core::fmt::Write;
    let st = state();
    let mut s = alloc::string::String::new();
    let _ = writeln!(s, "IP               MAC");
    for e in st.arp_cache.iter() {
        if e.2 {
            let _ = writeln!(s, "{}.{}.{}.{}   {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                e.0[0], e.0[1], e.0[2], e.0[3],
                e.1[0], e.1[1], e.1[2], e.1[3], e.1[4], e.1[5]);
        }
    }
    s
}

/// Send an ARP request for `ip` and wait briefly for the reply; returns MAC.
pub fn arp_resolve(ip: &[u8; 4]) -> Option<[u8; 6]> {
    if let Some(m) = arp_lookup(ip) {
        return Some(m);
    }
    send_arp_for(ip);
    let mut rx = [0u8; 256];
    let dl = timer::get_ticks() + 100;
    while timer::get_ticks() < dl {
        let n = net::rx_poll(&mut rx);
        if n > 0 {
            parse_arp_reply(&rx[..n as usize]);
            if let Some(m) = arp_lookup(ip) {
                return Some(m);
            }
        }
        idle_wait();
    }
    None
}

/// Render a /proc-style snapshot of network state.
pub fn render_info() -> alloc::string::String {
    use core::fmt::Write;
    let st = state();
    let nd = net::dev();
    let mut s = alloc::string::String::new();
    let _ = writeln!(
        s,
        "mac:        {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        nd.mac[0], nd.mac[1], nd.mac[2], nd.mac[3], nd.mac[4], nd.mac[5]
    );
    let _ = writeln!(s, "link:       {}", if nd.link_status & net::VIRTIO_NET_S_LINK_UP != 0 { "UP" } else { "DOWN" });
    let _ = writeln!(s, "ip:         {}.{}.{}.{}", st.my_ip[0], st.my_ip[1], st.my_ip[2], st.my_ip[3]);
    let _ = writeln!(s, "gateway:    {}.{}.{}.{}", st.gateway_ip[0], st.gateway_ip[1], st.gateway_ip[2], st.gateway_ip[3]);
    let _ = writeln!(s, "dhcp:       {}", if st.dhcp_acquired { "yes" } else { "no" });
    let _ = writeln!(s, "rx_packets: {}", nd.rx_packets);
    let _ = writeln!(s, "tx_packets: {}", nd.tx_packets);
    s
}

fn encode_qname(host: &str, out: &mut [u8]) -> usize {
    let mut p = 0;
    for label in host.split('.') {
        if label.is_empty() {
            continue;
        }
        out[p] = label.len() as u8;
        p += 1;
        out[p..p + label.len()].copy_from_slice(label.as_bytes());
        p += label.len();
    }
    out[p] = 0;
    p += 1;
    p
}

fn parse_dns_a(dns: &[u8]) -> Option<[u8; 4]> {
    if dns.len() < 12 {
        return None;
    }
    let ancount = ((dns[6] as u16) << 8) | dns[7] as u16;
    if ancount == 0 {
        return None;
    }
    let mut p = 12;
    // skip question QNAME
    while p < dns.len() && dns[p] != 0 {
        if dns[p] & 0xC0 == 0xC0 {
            p += 1;
            break;
        }
        p += dns[p] as usize + 1;
    }
    p += 1; // zero label
    p += 4; // qtype + qclass
    for _ in 0..ancount {
        if p >= dns.len() {
            break;
        }
        if dns[p] & 0xC0 == 0xC0 {
            p += 2;
        } else {
            while p < dns.len() && dns[p] != 0 {
                p += dns[p] as usize + 1;
            }
            p += 1;
        }
        if p + 10 > dns.len() {
            break;
        }
        let atype = ((dns[p] as u16) << 8) | dns[p + 1] as u16;
        let rdlen = ((dns[p + 8] as u16) << 8) | dns[p + 9] as u16;
        p += 10;
        if atype == 1 && rdlen == 4 && p + 4 <= dns.len() {
            return Some([dns[p], dns[p + 1], dns[p + 2], dns[p + 3]]);
        }
        p += rdlen as usize;
    }
    None
}

fn ensure_gateway_mac() -> bool {
    if state().have_gateway_mac {
        return true;
    }
    send_arp_probe();
    let mut rx = [0u8; 256];
    let dl = timer::get_ticks() + 100;
    while timer::get_ticks() < dl {
        let n = net::rx_poll(&mut rx);
        if n > 0 {
            parse_arp_reply(&rx[..n as usize]);
            if state().have_gateway_mac {
                return true;
            }
        }
        idle_wait();
    }
    state().have_gateway_mac
}

/// Resolve a hostname to an IPv4 address via slirp's DNS (10.0.2.3:53).
pub fn resolve(host: &str) -> Option<[u8; 4]> {
    if !ensure_gateway_mac() {
        return None;
    }
    let gw_mac = state().gateway_mac;
    let src_ip = state().my_ip;
    let dns_ip = [10u8, 0, 2, 3];

    let mut q = [0u8; 300];
    q[0] = 0x12;
    q[1] = 0x34; // id
    q[2] = 0x01; // RD
    q[5] = 1; // qdcount = 1
    let mut qlen = 12;
    qlen += encode_qname(host, &mut q[12..]);
    q[qlen + 1] = 1; // qtype = A
    q[qlen + 3] = 1; // qclass = IN
    qlen += 4;

    let mut frame = [0u8; 400];
    let flen = udp_build(&mut frame, &gw_mac, &src_ip, &dns_ip, 0x3535, 53, &q[..qlen]);
    net::tx(&frame[..flen]);

    let mut rx = [0u8; 600];
    let dl = timer::get_ticks() + 300;
    while timer::get_ticks() < dl {
        let n = net::rx_poll(&mut rx);
        if n as usize >= 14 + 20 + 8 && rx[12] == 0x08 && rx[13] == 0x00 {
            let ip = &rx[14..];
            if ip[9] == 17 {
                let ihl = (ip[0] & 0xF) as usize * 4;
                let udp = &ip[ihl..];
                let sport = ((udp[0] as u16) << 8) | udp[1] as u16;
                if sport == 53 {
                    if let Some(a) = parse_dns_a(&udp[8..(n as usize - 14 - ihl)]) {
                        return Some(a);
                    }
                }
            }
        }
        idle_wait();
    }
    None
}

fn tcp_checksum(src: &[u8; 4], dst: &[u8; 4], seg: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    sum += ((src[0] as u32) << 8) | src[1] as u32;
    sum += ((src[2] as u32) << 8) | src[3] as u32;
    sum += ((dst[0] as u32) << 8) | dst[1] as u32;
    sum += ((dst[2] as u32) << 8) | dst[3] as u32;
    sum += 6;
    sum += seg.len() as u32;
    let mut i = 0;
    while i + 1 < seg.len() { sum += ((seg[i] as u32) << 8) | seg[i + 1] as u32; i += 2; }
    if i < seg.len() { sum += (seg[i] as u32) << 8; }
    while sum >> 16 != 0 { sum = (sum & 0xFFFF) + (sum >> 16); }
    !(sum as u16)
}

#[allow(clippy::too_many_arguments)]
fn build_tcp(frame: &mut [u8], dst_mac: &[u8; 6], src_ip: &[u8; 4], dst_ip: &[u8; 4],
             sport: u16, dport: u16, seq: u32, ack: u32, flags: u8, payload: &[u8]) -> usize {
    let mac = my_mac();
    for i in 0..6 { frame[i] = dst_mac[i]; frame[6 + i] = mac[i]; }
    frame[12] = 0x08; frame[13] = 0x00;
    let tcp_len = 20 + payload.len();
    let ip_len = 20 + tcp_len;
    {
        let ip = &mut frame[14..34];
        ip[0] = 0x45;
        ip[2] = (ip_len >> 8) as u8; ip[3] = ip_len as u8;
        ip[6] = 0x40;
        ip[8] = 64; ip[9] = 6;
        for i in 0..4 { ip[12 + i] = src_ip[i]; ip[16 + i] = dst_ip[i]; }
        let c = inet_csum(&ip[..20]);
        ip[10] = (c >> 8) as u8; ip[11] = c as u8;
    }
    {
        let tcp = &mut frame[34..34 + tcp_len];
        tcp[0] = (sport >> 8) as u8; tcp[1] = sport as u8;
        tcp[2] = (dport >> 8) as u8; tcp[3] = dport as u8;
        tcp[4..8].copy_from_slice(&seq.to_be_bytes());
        tcp[8..12].copy_from_slice(&ack.to_be_bytes());
        tcp[12] = 5 << 4;
        tcp[13] = flags;
        tcp[14] = 0xFF; tcp[15] = 0xFF;
        tcp[16] = 0; tcp[17] = 0; // checksum field (zero before compute)
        tcp[18] = 0; tcp[19] = 0; // urgent pointer
        tcp[20..].copy_from_slice(payload);
        let c = tcp_checksum(src_ip, dst_ip, tcp);
        tcp[16] = (c >> 8) as u8; tcp[17] = c as u8;
    }
    34 + tcp_len
}

fn parse_tcp(rx: &[u8], lport: u16) -> Option<(u32, u32, u8, usize, usize)> {
    if rx.len() < 54 || rx[12] != 0x08 || rx[13] != 0x00 { return None; }
    let ip = &rx[14..];
    if ip[9] != 6 { return None; }
    let ihl = (ip[0] & 0xF) as usize * 4;
    let iptot = ((ip[2] as usize) << 8) | ip[3] as usize;
    let tcp = &ip[ihl..];
    let dport = ((tcp[2] as u16) << 8) | tcp[3] as u16;
    if dport != lport { return None; }
    let seq = u32::from_be_bytes([tcp[4], tcp[5], tcp[6], tcp[7]]);
    let ackn = u32::from_be_bytes([tcp[8], tcp[9], tcp[10], tcp[11]]);
    let flags = tcp[13];
    let doff = (tcp[12] >> 4) as usize * 4;
    Some((seq, ackn, flags, 14 + ihl + doff, iptot.saturating_sub(ihl + doff)))
}

/// Minimal HTTP/1.0 GET over a one-shot TCP connection; prints the response.
pub fn http_get(host: &str) -> bool {
    let ip = match resolve(host) { Some(a) => a, None => { uart::println("[TCP] DNS failed"); return false; } };
    let gw = state().gateway_mac;
    let src_ip = state().my_ip;
    let lport: u16 = 0x5566;
    let isn: u32 = 0x0001_0000;
    let mut frame = [0u8; 700];
    let mut rx = [0u8; 1600];

    let flen = build_tcp(&mut frame, &gw, &src_ip, &ip, lport, 80, isn, 0, 0x02, &[]);
    net::tx(&frame[..flen]);

    let mut server_seq = 0u32;
    let mut got = false;
    let dl = timer::get_ticks() + 300;
    while timer::get_ticks() < dl {
        let n = net::rx_poll(&mut rx) as usize;
        if n > 0 {
            if let Some((seq, _a, flags, _po, _pl)) = parse_tcp(&rx[..n], lport) {
                if flags & 0x12 == 0x12 { server_seq = seq; got = true; break; }
            }
        }
        idle_wait();
    }
    if !got { uart::println("[TCP] no SYN-ACK"); return false; }
    crate::kprintln!("[TCP] connected to {}.{}.{}.{}:80", ip[0], ip[1], ip[2], ip[3]);

    let my_seq = isn.wrapping_add(1);
    let their_next = server_seq.wrapping_add(1);
    // Finalize the handshake with a pure ACK, then send the request.
    let f = build_tcp(&mut frame, &gw, &src_ip, &ip, lport, 80, my_seq, their_next, 0x10, &[]);
    net::tx(&frame[..f]);

    let mut req = alloc::vec::Vec::new();
    req.extend_from_slice(b"GET / HTTP/1.0\r\nHost: ");
    req.extend_from_slice(host.as_bytes());
    req.extend_from_slice(b"\r\nConnection: close\r\n\r\n");
    let flen = build_tcp(&mut frame, &gw, &src_ip, &ip, lport, 80, my_seq, their_next, 0x18, &req);
    net::tx(&frame[..flen]);

    let mut printed = 0usize;
    let mut next_ack = their_next;
    let dl = timer::get_ticks() + 500;
    while timer::get_ticks() < dl && printed < 400 {
        let n = net::rx_poll(&mut rx) as usize;
        if n > 0 {
            if let Some((seq, _a, flags, po, pl)) = parse_tcp(&rx[..n], lport) {
                if pl > 0 && po + pl <= n {
                    let end = core::cmp::min(po + pl, po + (400 - printed));
                    for &c in &rx[po..end] { uart::putc(c); }
                    printed += end - po;
                    next_ack = seq.wrapping_add(pl as u32);
                    let f2 = build_tcp(&mut frame, &gw, &src_ip, &ip, lport, 80, my_seq, next_ack, 0x10, &[]);
                    net::tx(&frame[..f2]);
                }
                if flags & 0x01 != 0 { break; }
            }
        }
        idle_wait();
    }
    let flen = build_tcp(&mut frame, &gw, &src_ip, &ip, lport, 80, my_seq, next_ack, 0x11, &[]);
    net::tx(&frame[..flen]);
    uart::println("");
    printed > 0
}

/// Minimal SNTP client: query `host` (UDP 123) and return the Unix epoch
/// seconds from the server's transmit timestamp, or None.
pub fn ntp_query(host: &str) -> Option<u64> {
    let ip = resolve(host)?;
    if !ensure_gateway_mac() {
        return None;
    }
    let gw = state().gateway_mac;
    let src_ip = state().my_ip;
    // 48-byte NTP request: LI=0, VN=4, Mode=3 (client) -> 0x23.
    let mut pkt = [0u8; 48];
    pkt[0] = 0x23;
    let mut frame = [0u8; 200];
    let flen = udp_build(&mut frame, &gw, &src_ip, &ip, 0x4e54, 123, &pkt);
    net::tx(&frame[..flen]);

    let mut rx = [0u8; 600];
    let dl = timer::get_ticks() + 700;
    while timer::get_ticks() < dl {
        let n = net::rx_poll(&mut rx) as usize;
        if n >= 14 + 20 + 8 + 48 && rx[12] == 0x08 && rx[13] == 0x00 {
            let ip4 = &rx[14..];
            if ip4[9] == 17 {
                let ihl = (ip4[0] & 0xF) as usize * 4;
                let udp = &ip4[ihl..];
                let sport = ((udp[0] as u16) << 8) | udp[1] as u16;
                if sport == 123 {
                    let ntp = &udp[8..];
                    if ntp.len() >= 44 {
                        // Transmit timestamp seconds at offset 40 (NTP epoch 1900).
                        let secs = u32::from_be_bytes([ntp[40], ntp[41], ntp[42], ntp[43]]) as u64;
                        return Some(secs.wrapping_sub(2_208_988_800));
                    }
                }
            }
        }
        idle_wait();
    }
    None
}


/// SYS_NET_PING backend: ensure gateway MAC (ARP), send one ping, wait for the
/// echo reply, and return its TTL (or -1).
pub fn ping(seq: u16) -> i64 {
    let st = state();
    if !st.have_gateway_mac {
        send_arp_probe();
        let mut rx = [0u8; 256];
        let deadline = timer::get_ticks() + 100;
        while timer::get_ticks() < deadline {
            let n = net::rx_poll(&mut rx);
            if n > 0 {
                parse_arp_reply(&rx[..n as usize]);
                if state().have_gateway_mac {
                    break;
                }
            }
            idle_wait();
        }
    }
    if !state().have_gateway_mac {
        return -1;
    }
    if send_ping(seq) <= 0 {
        return -1;
    }
    let mut rx = [0u8; 256];
    let deadline = timer::get_ticks() + 200;
    while timer::get_ticks() < deadline {
        let n = net::rx_poll(&mut rx);
        if n >= 14 + 20 + 8 && rx[12] == 0x08 && rx[13] == 0x00 {
            let ip = &rx[14..];
            let icmp = &rx[34..];
            if ip[9] == 1 && icmp[0] == 0 {
                return ip[8] as i64; // TTL
            }
        }
        idle_wait();
    }
    -1
}

/// Boot-time network bring-up: DHCP, ARP, and an ICMP ping smoke test.
pub fn bringup() {
    if !net::dev().ready {
        return;
    }
    dhcp_acquire();

    if send_arp_probe() > 0 {
        let mut rx = [0u8; 256];
        let deadline = timer::get_ticks() + 100;
        while timer::get_ticks() < deadline {
            let n = net::rx_poll(&mut rx);
            if n > 0 {
                parse_arp_reply(&rx[..n as usize]);
                if state().have_gateway_mac {
                    break;
                }
            }
            idle_wait();
        }
    }
    if state().have_gateway_mac && send_ping(1) > 0 {
        let mut rx = [0u8; 256];
        let deadline = timer::get_ticks() + 200;
        while timer::get_ticks() < deadline {
            let n = net::rx_poll(&mut rx);
            if n >= 14 + 20 + 8 && rx[12] == 0x08 && rx[13] == 0x00 {
                let ip = &rx[14..];
                let icmp = &rx[34..];
                if ip[9] == 1 && icmp[0] == 0 {
                    kprintln!(
                        "[NET] PING reply from {}.{}.{}.{} ttl={} seq={} \\o/",
                        ip[12], ip[13], ip[14], ip[15], ip[8],
                        ((icmp[6] as u16) << 8) | icmp[7] as u16
                    );
                    break;
                }
            }
            idle_wait();
        }
    }
}
