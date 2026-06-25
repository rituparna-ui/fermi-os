// netclient: a bare-metal EL1 guest driving the hypervisor's emulated
// virtio-mmio NETWORK device. Exercises TWO virtqueues (RX=0, TX=1) and the
// 12-byte virtio-net header. The hyp is a loopback NIC, so a frame sent on TX
// comes back on RX.
//
// Sequence: handshake -> read MAC -> set up RX(q0) + TX(q1) -> post one RX
// buffer -> transmit a frame (net hdr + a tag byte) on TX -> the hyp loops it
// into the RX buffer + injects the RX IRQ -> we poll RX used.idx, read the
// looped frame back, verify the tag byte, print OK. Loops (incrementing tag).
//
// virtqueue + buffers at fixed IPAs:
//   RX: desc 0x40210000  avail 0x40211000  used 0x40212000  buf 0x40213000(256B)
//   TX: desc 0x40214000  avail 0x40215000  used 0x40216000  buf 0x40217000(256B)
// virtio-net MMIO @ 0x0A002000 (stage-2 invalid -> traps to EL2).

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.equ V, 0x0A002000
.equ R_MAGIC,0x000
.equ R_DEVICE_ID,0x008
.equ R_DRV_FEAT,0x020
.equ R_DRV_FEAT_SEL,0x024
.equ R_Q_SEL,0x030
.equ R_Q_NUM,0x038
.equ R_Q_READY,0x044
.equ R_Q_NOTIFY,0x050
.equ R_INT_ACK,0x064
.equ R_STATUS,0x070
.equ R_Q_DESC_LO,0x080
.equ R_Q_DESC_HI,0x084
.equ R_Q_DRV_LO,0x090
.equ R_Q_DRV_HI,0x094
.equ R_Q_DEV_LO,0x0A0
.equ R_Q_DEV_HI,0x0A4
.equ R_CONFIG,0x100

.equ ST_ACK,1
.equ ST_DRIVER,2
.equ ST_FEATURES_OK,8
.equ ST_DRIVER_OK,4

.equ RXD,0x40210000
.equ RXA,0x40211000
.equ RXU,0x40212000
.equ RXB,0x40213000
.equ TXD,0x40214000
.equ TXA,0x40215000
.equ TXU,0x40216000
.equ TXB,0x40217000
.equ QN,8
.equ HDR,12                    // virtio-net header length
.equ F_NEXT,1
.equ F_WRITE,2

.section .text.boot.net
.global _net_start
_net_start:
	ldr	x0,=0x40200000 ; mov sp,x0
	ldr	x28,=V
	adr	x19,m_banner ; bl n_puts

	// handshake
	mov	w1,#0 ; str w1,[x28,#R_STATUS]
	mov	w1,#ST_ACK ; str w1,[x28,#R_STATUS]
	mov	w1,#(ST_ACK|ST_DRIVER) ; str w1,[x28,#R_STATUS]
	mov	w1,#1 ; str w1,[x28,#R_DRV_FEAT_SEL] ; mov w1,#1 ; str w1,[x28,#R_DRV_FEAT]
	mov	w1,#0 ; str w1,[x28,#R_DRV_FEAT_SEL] ; mov w1,#0 ; str w1,[x28,#R_DRV_FEAT]
	mov	w1,#(ST_ACK|ST_DRIVER|ST_FEATURES_OK) ; str w1,[x28,#R_STATUS]

	// print MAC low 4 bytes
	adr	x19,m_mac ; bl n_puts
	ldr	w23,[x28,#R_CONFIG] ; bl n_word ; adr x19,m_nl ; bl n_puts

	// RX queue 0 setup
	mov	w1,#0 ; str w1,[x28,#R_Q_SEL]
	mov	w1,#QN ; str w1,[x28,#R_Q_NUM]
	ldr	x2,=RXD ; str w2,[x28,#R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2,=RXA ; str w2,[x28,#R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2,=RXU ; str w2,[x28,#R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1,#1 ; str w1,[x28,#R_Q_READY]
	// TX queue 1 setup
	mov	w1,#1 ; str w1,[x28,#R_Q_SEL]
	mov	w1,#QN ; str w1,[x28,#R_Q_NUM]
	ldr	x2,=TXD ; str w2,[x28,#R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2,=TXA ; str w2,[x28,#R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2,=TXU ; str w2,[x28,#R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1,#1 ; str w1,[x28,#R_Q_READY]
	mov	w1,#(ST_ACK|ST_DRIVER|ST_FEATURES_OK|ST_DRIVER_OK) ; str w1,[x28,#R_STATUS]

	mov	w24,#0                 // RX avail idx (free-running)
	mov	w25,#0                 // TX avail idx
	mov	w26,#0                 // last-seen RX used idx
	mov	w27,#0xC0              // tag byte

bigloop:
	// --- post one RX buffer (desc 0 @ RXB, WRITE, len 256) ---
	ldr	x4,=RXD
	ldr	x5,=RXB ; str x5,[x4,#0] ; mov w6,#256 ; str w6,[x4,#8]
	mov	w6,#F_WRITE ; strh w6,[x4,#12] ; strh wzr,[x4,#14]
	ldr	x4,=RXA ; strh wzr,[x4,#0]
	and	w8,w24,#(QN-1) ; add x8,x4,x8,lsl #1 ; strh wzr,[x8,#4]
	add	w24,w24,#1 ; strh w24,[x4,#2] ; dsb sy
	mov	w1,#0 ; str w1,[x28,#R_Q_NOTIFY]    // RX notify (posts buffer)

	// --- build TX frame @ TXB: [12B net hdr = 0][tag byte][pad] ---
	ldr	x4,=TXB
	mov	x5,#0
1:	cmp	x5,#HDR ; b.ge 2f ; strb wzr,[x4,x5] ; add x5,x5,#1 ; b 1b
2:	strb	w27,[x4,#HDR]          // payload[0] = tag
	add	w27,w27,#1
	// TX desc 0 = frame (RO, len HDR+1)
	ldr	x4,=TXD
	ldr	x5,=TXB ; str x5,[x4,#0] ; mov w6,#(HDR+1) ; str w6,[x4,#8]
	strh	wzr,[x4,#12] ; strh wzr,[x4,#14]   // flags=0 (RO, no next)
	ldr	x4,=TXA ; strh wzr,[x4,#0]
	and	w8,w25,#(QN-1) ; add x8,x4,x8,lsl #1 ; strh wzr,[x8,#4]
	add	w25,w25,#1 ; strh w25,[x4,#2] ; dsb sy
	mov	w1,#1 ; str w1,[x28,#R_Q_NOTIFY]    // TX notify -> loopback

	// --- wait for RX used.idx to advance (frame looped back) ---
	ldr	x4,=RXU ; mov x9,#0x2000000
3:	dsb sy ; ldrh w10,[x4,#2] ; cmp w10,w26 ; b.ne 4f
	subs	x9,x9,#1 ; b.ne 3b
	adr	x19,m_to ; bl n_puts ; b delay
4:	mov	w26,w10
	mov	w1,#1 ; str w1,[x28,#R_INT_ACK]
	// verify looped frame: RXB[HDR] == tag we sent (w27-1)
	ldr	x4,=RXB ; ldrb w6,[x4,#HDR]
	sub	w7,w27,#1 ; and w7,w7,#0xFF
	cmp	w6,w7 ; b.ne nfail
	adr	x19,m_ok ; bl n_puts ; mov w23,w6 ; bl n_hex2 ; adr x19,m_nl ; bl n_puts
	b	delay
nfail:
	adr	x19,m_fail ; bl n_puts
delay:
	mov	x20,#0x4000000
5:	subs x20,x20,#1 ; b.ne 5b
	mov	x0,#0 ; movk x0,#0xFE11,lsl #16 ; hvc #0   // YIELD
	b	bigloop

// helpers (clobber x9,x10,x12,x15)
n_puts:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
6:	ldrb w11,[x19],#1 ; cbz w11,7f
8:	ldr w12,[x10] ; tbnz w12,#5,8b ; str w11,[x9] ; b 6b
7:	ret
n_hex2:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
	lsr	w15,w23,#4 ; and w15,w15,#0xF ; cmp w15,#10 ; b.lt 9f ; add w15,w15,#('A'-10) ; b 10f
9:	add	w15,w15,#'0'
10:	ldr	w12,[x10] ; tbnz w12,#5,10b ; str w15,[x9]
	and	w15,w23,#0xF ; cmp w15,#10 ; b.lt 11f ; add w15,w15,#('A'-10) ; b 12f
11:	add	w15,w15,#'0'
12:	ldr	w12,[x10] ; tbnz w12,#5,12b ; str w15,[x9] ; ret
// print w23 as 8 hex digits (a 32-bit word), high nibble first
n_word:
	mov	x16,x30
	mov	w13,w23                // save
	lsr	w23,w13,#24 ; bl n_hex2
	lsr	w23,w13,#16 ; bl n_hex2
	lsr	w23,w13,#8  ; bl n_hex2
	mov	w23,w13     ; bl n_hex2
	mov	x30,x16 ; ret

.section .rodata.net
m_banner: .asciz "\r\n[netclient] driving virtio-mmio NET device (loopback)\r\n"
m_mac:    .asciz "[netclient] MAC low word = 0x"
m_ok:     .asciz "[netclient] TX->RX loopback OK, tag=0x"
m_fail:   .asciz "[netclient] loopback MISMATCH!\r\n"
m_to:     .asciz "[netclient] loopback timed out\r\n"
m_nl:     .asciz "\r\n"
