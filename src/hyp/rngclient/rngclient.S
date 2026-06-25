// rngclient: a bare-metal EL1 guest that drives the hypervisor's emulated
// virtio-mmio ENTROPY device through the STANDARD virtio 1.x register interface
// + a split virtqueue. Proves the paravirtual device transport end to end:
// discover, negotiate, set up a queue, request random bytes, get them filled.
//
// It uses a single queue (queue 0) with N=4 descriptors and one WRITE buffer.
// All virtqueue structures live at fixed IPAs in this guest's RAM:
//   desc table  @ 0x40210000  (N*16 = 64 bytes)
//   avail ring  @ 0x40211000
//   used  ring  @ 0x40212000
//   rng buffer  @ 0x40213000  (8 bytes we ask the device to fill)
// The device's MMIO block is at IPA 0x0A000000 (stage-2-invalid -> traps to EL2).
//
// Sequence: reset -> ACK -> DRIVER -> (features) -> FEATURES_OK -> queue setup
// -> DRIVER_OK -> publish 1 buffer -> QueueNotify -> spin until used.idx moves
// -> print the 8 random bytes. Repeats every ~1s.

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.equ V, 0x0A000000              // virtio-mmio base
.equ R_MAGIC,        0x000
.equ R_VERSION,      0x004
.equ R_DEVICE_ID,    0x008
.equ R_DEV_FEAT_SEL, 0x014
.equ R_DRV_FEAT,     0x020
.equ R_DRV_FEAT_SEL, 0x024
.equ R_Q_SEL,        0x030
.equ R_Q_NUM_MAX,    0x034
.equ R_Q_NUM,        0x038
.equ R_Q_READY,      0x044
.equ R_Q_NOTIFY,     0x050
.equ R_INT_STATUS,   0x060
.equ R_INT_ACK,      0x064
.equ R_STATUS,       0x070
.equ R_Q_DESC_LO,    0x080
.equ R_Q_DESC_HI,    0x084
.equ R_Q_DRV_LO,     0x090
.equ R_Q_DRV_HI,     0x094
.equ R_Q_DEV_LO,     0x0A0
.equ R_Q_DEV_HI,     0x0A4

.equ ST_ACK,        1
.equ ST_DRIVER,     2
.equ ST_DRIVER_OK,  4
.equ ST_FEATURES_OK, 8

.equ DESC_ADDR, 0x40210000
.equ AVAIL_ADDR, 0x40211000
.equ USED_ADDR,  0x40212000
.equ BUF_ADDR,   0x40213000
.equ QN, 4                      // queue size
.equ BUFLEN, 8                  // random bytes per request

.equ DESC_F_WRITE, 2

.section .text.boot.rng
.global _rng_start
_rng_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	mov	x28, #V                  // x28 = virtio base (kept across calls)

	adr	x19, m_banner
	bl	r_puts

	// --- handshake ---
	mov	w1, #0                   // Status = 0 (reset)
	str	w1, [x28, #R_STATUS]
	mov	w1, #ST_ACK
	str	w1, [x28, #R_STATUS]
	mov	w1, #(ST_ACK|ST_DRIVER)
	str	w1, [x28, #R_STATUS]
	// accept VERSION_1 (bit 32): DriverFeaturesSel=1, DriverFeatures=1; sel=0,=0
	mov	w1, #1 ; str w1, [x28, #R_DRV_FEAT_SEL]
	mov	w1, #1 ; str w1, [x28, #R_DRV_FEAT]
	mov	w1, #0 ; str w1, [x28, #R_DRV_FEAT_SEL]
	mov	w1, #0 ; str w1, [x28, #R_DRV_FEAT]
	mov	w1, #(ST_ACK|ST_DRIVER|ST_FEATURES_OK)
	str	w1, [x28, #R_STATUS]

	// --- queue 0 setup ---
	mov	w1, #0 ; str w1, [x28, #R_Q_SEL]
	mov	w1, #QN ; str w1, [x28, #R_Q_NUM]
	ldr	x2, =DESC_ADDR  ; str w2, [x28, #R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2, =AVAIL_ADDR ; str w2, [x28, #R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2, =USED_ADDR  ; str w2, [x28, #R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1, #1 ; str w1, [x28, #R_Q_READY]
	mov	w1, #(ST_ACK|ST_DRIVER|ST_FEATURES_OK|ST_DRIVER_OK)
	str	w1, [x28, #R_STATUS]

	// w27 = last-seen used.idx, w26 = our running avail.idx (free-running)
	mov	w27, #0
	mov	w26, #0

loop:
	// --- build descriptor 0: WRITE buffer @ BUF_ADDR, len BUFLEN ---
	ldr	x4, =DESC_ADDR
	ldr	x5, =BUF_ADDR
	str	x5, [x4, #0]             // desc[0].addr (le64)
	mov	w6, #BUFLEN
	str	w6, [x4, #8]             // desc[0].len
	mov	w6, #DESC_F_WRITE
	strh	w6, [x4, #12]           // desc[0].flags
	strh	wzr, [x4, #14]          // desc[0].next = 0

	// --- avail ring: publish head desc 0 at slot (avail.idx % QN), then
	// increment the FREE-RUNNING avail.idx so the device sees a new entry. ---
	ldr	x4, =AVAIL_ADDR
	strh	wzr, [x4, #0]           // avail.flags = 0
	and	w8, w26, #(QN-1)        // slot = avail.idx % QN
	add	x8, x4, x8, lsl #1      // &avail.ring[slot]  (ring at +4, 2 bytes each)
	strh	wzr, [x8, #4]          // avail.ring[slot] = desc 0
	add	w26, w26, #1           // avail.idx++
	strh	w26, [x4, #2]          // publish new avail.idx
	dsb	sy

	// --- kick: QueueNotify = 0 ---
	mov	w1, #0
	str	w1, [x28, #R_Q_NOTIFY]

	// --- wait for the device to advance used.idx past our last-seen ---
	ldr	x4, =USED_ADDR
	mov	x8, #0x2000000          // spin bound
1:	dsb	sy
	ldrh	w9, [x4, #2]            // used.idx
	cmp	w9, w27
	b.ne	2f
	subs	x8, x8, #1
	b.ne	1b
	adr	x19, m_timeout
	bl	r_puts
	b	delay
2:	mov	w27, w9                  // update last-seen
	// ACK the device interrupt (we polled, but clear status anyway)
	mov	w1, #1 ; str w1, [x28, #R_INT_ACK]

	// --- print the 8 random bytes from BUF_ADDR ---
	// Use callee-stable x24 (buffer ptr) and x25 (index): the UART helpers
	// clobber x9/x10/x12/x15, so the loop counter must NOT live in those.
	adr	x19, m_got
	bl	r_puts
	ldr	x24, =BUF_ADDR
	mov	x25, #0
3:	cmp	x25, #BUFLEN
	b.ge	4f
	ldrb	w23, [x24, x25]
	bl	r_puthex2
	mov	w0, #' ' ; bl r_putc
	add	x25, x25, #1
	b	3b
4:	adr	x19, m_nl
	bl	r_puts

delay:
	mov	x20, #0x4000000
5:	subs	x20, x20, #1
	b.ne	5b
	// yield to other VMs
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16    // HVC_FERMI_YIELD
	hvc	#0
	b	loop

// --- helpers ---
r_putc:
	ldr	x9, =UART_DR ; ldr x10, =UART_FR
6:	ldr	w12,[x10] ; tbnz w12,#5,6b ; str w0,[x9] ; ret
r_puts:
	ldr	x9, =UART_DR ; ldr x10, =UART_FR
7:	ldrb	w11,[x19],#1 ; cbz w11,8f
9:	ldr	w12,[x10] ; tbnz w12,#5,9b ; str w11,[x9] ; b 7b
8:	ret
r_puthex2:
	ldr	x9, =UART_DR ; ldr x10, =UART_FR
	lsr	w15,w23,#4 ; and w15,w15,#0xF ; cmp w15,#10 ; b.lt 10f ; add w15,w15,#('A'-10) ; b 11f
10:	add	w15,w15,#'0'
11:	ldr	w12,[x10] ; tbnz w12,#5,11b ; str w15,[x9]
	and	w15,w23,#0xF ; cmp w15,#10 ; b.lt 12f ; add w15,w15,#('A'-10) ; b 13f
12:	add	w15,w15,#'0'
13:	ldr	w12,[x10] ; tbnz w12,#5,13b ; str w15,[x9] ; ret

.section .rodata.rng
m_banner:  .asciz "\r\n[rngclient] driving virtio-mmio entropy device\r\n"
m_got:     .asciz "[rngclient] virtio-rng bytes: "
m_timeout: .asciz "[rngclient] virtio request timed out\r\n"
m_nl:      .asciz "\r\n"
