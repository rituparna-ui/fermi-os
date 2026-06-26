// balloonclient: a bare-metal EL1 guest that drives the hypervisor's emulated
// virtio-mmio MEMORY-BALLOON device (DeviceID 5) through the standard virtio 1.x
// register interface + TWO split virtqueues (0=inflateq, 1=deflateq).
//
// The device self-drives its target: a CNTPCT clock in the hyp retargets
// num_pages between an inflate goal (4 MiB = 1024 pages) and 0, raising a
// config-change interrupt. This guest POLLS config (it doesn't use the IRQ):
// each loop it reads num_pages with the ConfigGeneration protocol, then inflates
// or deflates the difference by putting an array of le32 PFNs on the appropriate
// queue and notifying. The hyp zeroes donated pages on inflate (honest model:
// the page is NOT returned to the host — fixed stage-2 — only zeroed + counted).
//
// Buffers are device-READ (we WRITE the PFN array, device reads it): the
// descriptor has flags=0 (no WRITE bit), and the device posts used.len=0.
//
// Fixed IPAs in this guest's 16 MiB RAM (all inside [0x40000000,0x41000000)):
//   inflateq:  desc 0x40210000, avail 0x40211000, used 0x40212000
//   deflateq:  desc 0x40220000, avail 0x40221000, used 0x40222000
//   PFN buffer 0x40230000 (256 * 4 = 1024 bytes; one buffer, reused)
//   donated page pool base 0x40400000 (the pages we hand to the device)
//   QN = 8 descriptors per queue.
// The device MMIO block is at IPA 0x0A004000 (stage-2-invalid -> traps to EL2).
//
// Links flat at 0x40000000; _balloon_start is the first byte. Embedded via
// balloonclient_blob.S.

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.equ V, 0x0A004000              // virtio-mmio balloon base
.equ R_MAGIC,        0x000
.equ R_DEVICE_ID,    0x008
.equ R_DEV_FEAT_SEL, 0x014
.equ R_DRV_FEAT,     0x020
.equ R_DRV_FEAT_SEL, 0x024
.equ R_Q_SEL,        0x030
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
.equ R_CONFIG_GEN,   0x0FC
.equ R_NUM_PAGES,    0x100      // RO: device target
.equ R_ACTUAL,       0x104      // RW: pages we report in the balloon

.equ ST_ACK,        1
.equ ST_DRIVER,     2
.equ ST_DRIVER_OK,  4
.equ ST_FEATURES_OK, 8

.equ INF_DESC,  0x40210000
.equ INF_AVAIL, 0x40211000
.equ INF_USED,  0x40212000
.equ DEF_DESC,  0x40220000
.equ DEF_AVAIL, 0x40221000
.equ DEF_USED,  0x40222000
.equ PFN_BUF,   0x40230000
.equ POOL_BASE, 0x40400000      // donated pages start here
.equ POOL_PFN0, 0x40400         // POOL_BASE >> 12
.equ QN, 8                      // descriptors per queue
.equ BATCH, 256                 // max PFNs per inflate/deflate buffer

.section .text.boot.balloon
.global _balloon_start
_balloon_start:
	ldr	x0, =0x40200000
	mov	sp, x0
	ldr	x28, =V                  // x28 = balloon MMIO base (kept across calls)

	adr	x19, m_banner
	bl	b_puts

	// --- handshake ---
	mov	w1, #0 ; str w1, [x28, #R_STATUS]            // reset
	mov	w1, #ST_ACK ; str w1, [x28, #R_STATUS]
	mov	w1, #(ST_ACK|ST_DRIVER) ; str w1, [x28, #R_STATUS]
	// accept VERSION_1 (bit32): DriverFeaturesSel=1,DriverFeatures=1; sel=0,=0
	mov	w1, #1 ; str w1, [x28, #R_DRV_FEAT_SEL]
	mov	w1, #1 ; str w1, [x28, #R_DRV_FEAT]
	mov	w1, #0 ; str w1, [x28, #R_DRV_FEAT_SEL]
	mov	w1, #0 ; str w1, [x28, #R_DRV_FEAT]
	mov	w1, #(ST_ACK|ST_DRIVER|ST_FEATURES_OK) ; str w1, [x28, #R_STATUS]

	// --- queue 0 (inflateq) setup ---
	mov	w1, #0 ; str w1, [x28, #R_Q_SEL]
	mov	w1, #QN ; str w1, [x28, #R_Q_NUM]
	ldr	x2,=INF_DESC  ; str w2,[x28,#R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2,=INF_AVAIL ; str w2,[x28,#R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2,=INF_USED  ; str w2,[x28,#R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1, #1 ; str w1, [x28, #R_Q_READY]

	// --- queue 1 (deflateq) setup ---
	mov	w1, #1 ; str w1, [x28, #R_Q_SEL]
	mov	w1, #QN ; str w1, [x28, #R_Q_NUM]
	ldr	x2,=DEF_DESC  ; str w2,[x28,#R_Q_DESC_LO] ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DESC_HI]
	ldr	x2,=DEF_AVAIL ; str w2,[x28,#R_Q_DRV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DRV_HI]
	ldr	x2,=DEF_USED  ; str w2,[x28,#R_Q_DEV_LO]  ; lsr x3,x2,#32 ; str w3,[x28,#R_Q_DEV_HI]
	mov	w1, #1 ; str w1, [x28, #R_Q_READY]

	mov	w1, #(ST_ACK|ST_DRIVER|ST_FEATURES_OK|ST_DRIVER_OK)
	str	w1, [x28, #R_STATUS]

	// Callee-stable running state (survives the UART helpers which clobber
	// x9/x10/x11/x12/x15):
	//   w24 = actual (pages we have in the balloon)
	//   w25 = inflateq free-running avail.idx
	//   w26 = deflateq free-running avail.idx
	//   w27 = inflateq last-seen used.idx
	//   w22 = deflateq last-seen used.idx
	mov	w24, #0
	mov	w25, #0
	mov	w26, #0
	mov	w27, #0
	mov	w22, #0

main_loop:
	// --- read num_pages with the ConfigGeneration protocol ---
	// g0=gen ; target=num_pages ; g1=gen ; retry if g0!=g1.
1:	ldr	w13, [x28, #R_CONFIG_GEN]
	ldr	w21, [x28, #R_NUM_PAGES]     // w21 = target (callee-stable)
	ldr	w14, [x28, #R_CONFIG_GEN]
	cmp	w13, w14
	b.ne	1b

	cmp	w21, w24
	b.hi	do_inflate                  // target > actual -> inflate
	b.lo	do_deflate                  // target < actual -> deflate
	b	ack_and_wait                // equal -> nothing to do

do_inflate:
	// count = min(target-actual, BATCH); donate PFNs [actual .. actual+count)
	sub	w23, w21, w24               // w23 = count (callee-stable)
	cmp	w23, #BATCH
	b.ls	2f
	mov	w23, #BATCH
2:	// build PFN array at PFN_BUF: pfn[k] = POOL_PFN0 + actual + k
	ldr	x4, =PFN_BUF
	mov	w5, #0                      // k
3:	cmp	w5, w23
	b.ge	4f
	ldr	w6, =POOL_PFN0
	add	w6, w6, w24                 // + actual
	add	w6, w6, w5                  // + k
	str	w6, [x4, w5, uxtw #2]       // pfn array is le32
	add	w5, w5, #1
	b	3b
4:	// descriptor 0: addr=PFN_BUF, len=count*4, flags=0 (device-READ), next=0
	ldr	x7, =INF_DESC
	ldr	x8, =PFN_BUF
	str	x8, [x7, #0]                // desc[0].addr (le64)
	lsl	w9, w23, #2                 // len = count*4
	str	w9, [x7, #8]                // desc[0].len
	strh	wzr, [x7, #12]             // desc[0].flags = 0 (no WRITE)
	strh	wzr, [x7, #14]             // desc[0].next = 0
	// publish on inflateq avail ring (slot = avail.idx % QN). Barrier AFTER the
	// PFN-array + descriptor stores, BEFORE the avail.idx bump.
	dsb	sy
	ldr	x10, =INF_AVAIL
	strh	wzr, [x10, #0]             // avail.flags = 0
	and	w11, w25, #(QN-1)
	add	x12, x10, x11, lsl #1
	strh	wzr, [x12, #4]            // avail.ring[slot] = desc 0
	add	w25, w25, #1
	strh	w25, [x10, #2]            // publish new avail.idx
	dsb	sy
	mov	w1, #0 ; str w1, [x28, #R_Q_NOTIFY]   // kick inflateq (queue 0)
	// wait for inflateq used.idx to advance past last-seen
	ldr	x10, =INF_USED
	mov	x12, #0x2000000
5:	dsb	sy
	ldrh	w9, [x10, #2]
	cmp	w9, w27
	b.ne	6f
	subs	x12, x12, #1
	b.ne	5b
	adr	x19, m_timeout ; bl b_puts
	b	ack_and_wait
6:	mov	w27, w9                     // update last-seen inflateq used.idx
	add	w24, w24, w23               // actual += count
	str	w24, [x28, #R_ACTUAL]       // report actual to device
	adr	x19, m_inf ; bl b_puts
	mov	w20, w23 ; bl b_hex4        // pages this batch
	adr	x19, m_act ; bl b_puts
	mov	w20, w24 ; bl b_hex4        // new actual
	adr	x19, m_nl ; bl b_puts
	b	ack_and_wait

do_deflate:
	// count = min(actual-target, BATCH); reclaim PFNs [actual-count .. actual)
	sub	w23, w24, w21               // w23 = count
	cmp	w23, #BATCH
	b.ls	7f
	mov	w23, #BATCH
7:	// base PFN index for the pages we're returning = actual - count
	sub	w15, w24, w23               // w15 = first returned page index
	ldr	x4, =PFN_BUF
	mov	w5, #0
8:	cmp	w5, w23
	b.ge	9f
	ldr	w6, =POOL_PFN0
	add	w6, w6, w15
	add	w6, w6, w5
	str	w6, [x4, w5, uxtw #2]
	add	w5, w5, #1
	b	8b
9:	ldr	x7, =DEF_DESC
	ldr	x8, =PFN_BUF
	str	x8, [x7, #0]
	lsl	w9, w23, #2
	str	w9, [x7, #8]
	strh	wzr, [x7, #12]            // flags = 0 (device-READ)
	strh	wzr, [x7, #14]
	dsb	sy
	ldr	x10, =DEF_AVAIL
	strh	wzr, [x10, #0]
	and	w11, w26, #(QN-1)
	add	x12, x10, x11, lsl #1
	strh	wzr, [x12, #4]
	add	w26, w26, #1
	strh	w26, [x10, #2]
	dsb	sy
	mov	w1, #1 ; str w1, [x28, #R_Q_NOTIFY]   // kick deflateq (queue 1)
	ldr	x10, =DEF_USED
	mov	x12, #0x2000000
10:	dsb	sy
	ldrh	w9, [x10, #2]
	cmp	w9, w22
	b.ne	11f
	subs	x12, x12, #1
	b.ne	10b
	adr	x19, m_timeout ; bl b_puts
	b	ack_and_wait
11:	mov	w22, w9                    // update last-seen deflateq used.idx
	sub	w24, w24, w23              // actual -= count
	str	w24, [x28, #R_ACTUAL]
	adr	x19, m_def ; bl b_puts
	mov	w20, w23 ; bl b_hex4
	adr	x19, m_act ; bl b_puts
	mov	w20, w24 ; bl b_hex4
	adr	x19, m_nl ; bl b_puts
	b	ack_and_wait

ack_and_wait:
	// ACK any device interrupt (we polled; clear both VRING + CONFIG bits).
	ldr	w9, [x28, #R_INT_STATUS]
	str	w9, [x28, #R_INT_ACK]
	// delay, then yield to other VMs (HVC_FERMI_YIELD).
	mov	x20, #0x2000000
12:	subs x20, x20, #1 ; b.ne 12b
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16
	hvc	#0
	b	main_loop

// --- helpers (clobber x9/x10/x11/x12/x15) ---
b_puts:
	ldr	x9, =UART_DR ; ldr x10, =UART_FR
13:	ldrb w11,[x19],#1 ; cbz w11,14f
15:	ldr w12,[x10] ; tbnz w12,#5,15b ; str w11,[x9] ; b 13b
14:	ret
// print w20 as 4 hex nibbles (16-bit page count is plenty)
b_hex4:
	ldr	x9, =UART_DR ; ldr x10, =UART_FR
	mov	w15, #12
16:	lsr	w11, w20, w15
	and	w11, w11, #0xF
	cmp	w11, #10 ; b.lt 17f ; add w11, w11, #('A'-10) ; b 18f
17:	add	w11, w11, #'0'
18:	ldr	w12,[x10] ; tbnz w12,#5,18b ; str w11,[x9]
	subs	w15, w15, #4
	b.ge	16b
	ret

.section .rodata.balloon
m_banner:  .asciz "\r\n[balloonclient] driving virtio-mmio MEMORY-BALLOON device\r\n"
m_inf:     .asciz "[balloonclient] inflated 0x"
m_def:     .asciz "[balloonclient] deflated 0x"
m_act:     .asciz " pages, actual=0x"
m_timeout: .asciz "[balloonclient] virtio request timed out\r\n"
m_nl:      .asciz "\r\n"
