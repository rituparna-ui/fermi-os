// blkclient: a bare-metal EL1 guest that drives the hypervisor's emulated
// virtio-mmio BLOCK device. Exercises the full virtio request shape (3-desc
// chain: header -> data -> status) and bidirectional transfer:
//   1. WRITE a known pattern to sector 1 (T_OUT)
//   2. READ sector 1 back (T_IN)
//   3. verify the bytes match -> print OK/FAIL
// then loops (incrementing the pattern) to show the disk is real + persistent.
//
// virtqueue (queue 0, N=8) + buffers at fixed IPAs in this guest's RAM:
//   desc   @ 0x40210000   avail @ 0x40211000   used @ 0x40212000
//   hdr    @ 0x40213000 (16B)   data @ 0x40213100 (512B)   status @ 0x40213400 (1B)
// virtio-blk MMIO block @ 0x0A001000 (stage-2 invalid -> traps to EL2).

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.equ V, 0x0A001000             // virtio-blk mmio base
.equ R_MAGIC,      0x000
.equ R_DEVICE_ID,  0x008
.equ R_DRV_FEAT,   0x020
.equ R_DRV_FEAT_SEL, 0x024
.equ R_Q_SEL,      0x030
.equ R_Q_NUM,      0x038
.equ R_Q_READY,    0x044
.equ R_Q_NOTIFY,   0x050
.equ R_INT_ACK,    0x064
.equ R_STATUS,     0x070
.equ R_Q_DESC_LO,  0x080
.equ R_Q_DESC_HI,  0x084
.equ R_Q_DRV_LO,   0x090
.equ R_Q_DRV_HI,   0x094
.equ R_Q_DEV_LO,   0x0A0
.equ R_Q_DEV_HI,   0x0A4
.equ R_CONFIG,     0x100

.equ ST_ACK, 1
.equ ST_DRIVER, 2
.equ ST_FEATURES_OK, 8
.equ ST_DRIVER_OK, 4

.equ DESC,  0x40210000
.equ AVAIL, 0x40211000
.equ USED,  0x40212000
.equ HDR,   0x40213000
.equ DATA,  0x40213100
.equ STAT,  0x40213400
.equ QN, 8

.equ T_IN, 0
.equ T_OUT, 1
.equ F_NEXT, 1
.equ F_WRITE, 2
.equ SECTOR, 1                 // operate on sector 1

.section .text.boot.blk
.global _blk_start
_blk_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	ldr	x28, =V
	adr	x19, m_banner
	bl	b_puts

	// handshake
	mov	w1,#0 ; str w1,[x28,#R_STATUS]
	mov	w1,#ST_ACK ; str w1,[x28,#R_STATUS]
	mov	w1,#(ST_ACK|ST_DRIVER) ; str w1,[x28,#R_STATUS]
	mov	w1,#1 ; str w1,[x28,#R_DRV_FEAT_SEL]
	mov	w1,#1 ; str w1,[x28,#R_DRV_FEAT]
	mov	w1,#0 ; str w1,[x28,#R_DRV_FEAT_SEL]
	mov	w1,#0 ; str w1,[x28,#R_DRV_FEAT]
	mov	w1,#(ST_ACK|ST_DRIVER|ST_FEATURES_OK) ; str w1,[x28,#R_STATUS]

	// read + print disk capacity (sectors)
	adr	x19, m_cap ; bl b_puts
	ldr	w23, [x28, #R_CONFIG] ; bl b_puthex2
	adr	x19, m_nl ; bl b_puts

	// queue 0 setup
	mov	w1,#0 ; str w1,[x28,#R_Q_SEL]
	mov	w1,#QN ; str w1,[x28,#R_Q_NUM]
	ldr	x2,=DESC  ; str w2,[x28,#R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2,=AVAIL ; str w2,[x28,#R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2,=USED  ; str w2,[x28,#R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1,#1 ; str w1,[x28,#R_Q_READY]
	mov	w1,#(ST_ACK|ST_DRIVER|ST_FEATURES_OK|ST_DRIVER_OK) ; str w1,[x28,#R_STATUS]

	mov	w27,#0                 // last-seen used.idx
	mov	w26,#0                 // free-running avail.idx
	mov	w25,#0xA0              // pattern seed byte

bigloop:
	// fill DATA with the pattern (512 bytes of w25, then w25++)
	ldr	x4,=DATA
	mov	x5,#0
1:	cmp	x5,#512 ; b.ge 2f
	strb	w25,[x4,x5] ; add x5,x5,#1 ; b 1b
2:	add	w25,w25,#1

	// --- WRITE request (T_OUT): hdr -> data(RO) -> status(WO) ---
	mov	w0,#T_OUT
	bl	do_request

	// clear DATA so the read must refill it
	ldr	x4,=DATA ; mov x5,#0
3:	cmp	x5,#512 ; b.ge 4f
	strb	wzr,[x4,x5] ; add x5,x5,#1 ; b 3b
4:
	// --- READ request (T_IN): hdr -> data(WO) -> status(WO) ---
	mov	w0,#T_IN
	bl	do_request

	// verify DATA[0] == expected pattern (w25-1)
	ldr	x4,=DATA
	ldrb	w6,[x4,#0]
	sub	w7,w25,#1
	and	w7,w7,#0xFF
	cmp	w6,w7
	b.ne	vfail
	adr	x19,m_ok ; bl b_puts
	mov	w23,w6 ; bl b_puthex2
	adr	x19,m_nl ; bl b_puts
	b	vdone
vfail:
	adr	x19,m_fail ; bl b_puts
vdone:
	// delay + yield
	mov	x20,#0x4000000
5:	subs x20,x20,#1 ; b.ne 5b
	mov	x0,#0 ; movk x0,#0xFE11,lsl #16 ; hvc #0   // YIELD
	b	bigloop

// do_request: w0 = type (T_IN/T_OUT). Builds the 3-desc chain, publishes, kicks,
// waits for completion. Data descriptor flags depend on type (WRITE for reads).
do_request:
	mov	x16,x30                // save return (helpers use x30)
	// header @ HDR: {type=w0, reserved=0, sector=SECTOR}
	ldr	x4,=HDR
	str	w0,[x4,#0]
	str	wzr,[x4,#4]
	mov	x6,#SECTOR ; str x6,[x4,#8]
	mov	w17,w0                 // remember type

	// desc[0] = header (RO, NEXT->1)
	ldr	x4,=DESC
	ldr	x5,=HDR ; str x5,[x4,#0] ; mov w6,#16 ; str w6,[x4,#8]
	mov	w6,#F_NEXT ; strh w6,[x4,#12] ; mov w6,#1 ; strh w6,[x4,#14]
	// desc[1] = data (NEXT->2; WRITE iff read request)
	ldr	x5,=DATA ; str x5,[x4,#16] ; mov w6,#512 ; str w6,[x4,#24]
	mov	w6,#F_NEXT
	cmp	w17,#T_IN
	b.ne	6f
	orr	w6,w6,#F_WRITE         // read => device writes the data buffer
6:	strh	w6,[x4,#28] ; mov w6,#2 ; strh w6,[x4,#30]
	// desc[2] = status (WO, no NEXT)
	ldr	x5,=STAT ; str x5,[x4,#32] ; mov w6,#1 ; str w6,[x4,#40]
	mov	w6,#F_WRITE ; strh w6,[x4,#44] ; strh wzr,[x4,#46]

	// avail: publish head 0 at slot (avail.idx % QN), bump avail.idx
	ldr	x4,=AVAIL
	strh	wzr,[x4,#0]
	and	w8,w26,#(QN-1) ; add x8,x4,x8,lsl #1 ; strh wzr,[x8,#4]
	add	w26,w26,#1 ; strh w26,[x4,#2]
	dsb	sy
	// kick
	mov	w1,#0 ; str w1,[x28,#R_Q_NOTIFY]
	// wait for used.idx to advance
	ldr	x4,=USED ; mov x9,#0x2000000
7:	dsb sy ; ldrh w10,[x4,#2] ; cmp w10,w27 ; b.ne 8f
	subs	x9,x9,#1 ; b.ne 7b
	adr	x19,m_to ; bl b_puts ; b 9f
8:	mov	w27,w10
	mov	w1,#1 ; str w1,[x28,#R_INT_ACK]
9:	mov	x30,x16
	ret

// helpers (clobber x9,x10,x12,x15)
b_puts:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
10:	ldrb w11,[x19],#1 ; cbz w11,11f
12:	ldr w12,[x10] ; tbnz w12,#5,12b ; str w11,[x9] ; b 10b
11:	ret
b_puthex2:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
	lsr	w15,w23,#4 ; and w15,w15,#0xF ; cmp w15,#10 ; b.lt 13f ; add w15,w15,#('A'-10) ; b 14f
13:	add	w15,w15,#'0'
14:	ldr	w12,[x10] ; tbnz w12,#5,14b ; str w15,[x9]
	and	w15,w23,#0xF ; cmp w15,#10 ; b.lt 15f ; add w15,w15,#('A'-10) ; b 16f
15:	add	w15,w15,#'0'
16:	ldr	w12,[x10] ; tbnz w12,#5,16b ; str w15,[x9] ; ret

.section .rodata.blk
m_banner: .asciz "\r\n[blkclient] driving virtio-mmio BLOCK device\r\n"
m_cap:    .asciz "[blkclient] disk capacity (sectors) = 0x"
m_ok:     .asciz "[blkclient] write+read sector 1 OK, byte=0x"
m_fail:   .asciz "[blkclient] write+read MISMATCH!\r\n"
m_to:     .asciz "[blkclient] request timed out\r\n"
m_nl:     .asciz "\r\n"
