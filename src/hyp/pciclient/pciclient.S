// pciclient: a bare-metal EL1 guest that enumerates the hypervisor's emulated
// virtual PCI bus over ECAM and configures the one device it finds — the
// standard PCI bring-up flow:
//   1. scan slots 0..7 on bus 0, read VendorID/DeviceID (0xFFFFFFFF = empty)
//   2. for the found device: size BAR0 (write 0xFFFFFFFF, read back the mask)
//   3. assign BAR0 a base address
//   4. enable Memory Space + Bus Master in the Command register
//   5. print vendor/device, BAR size, assigned base
// then idles (yields). Proves config-space reads/writes + BAR sizing work.
//
// ECAM @ IPA 0x0A003000 (stage-2 invalid -> traps to EL2). ECAM address =
// base | bus<<20 | slot<<15 | func<<12 | reg. Our 4 KiB window = bus0/slot0.

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018
.equ ECAM, 0x0A003000          // bus0/slot0/func0 config space

.equ CFG_VENDOR,  0x00
.equ CFG_COMMAND, 0x04
.equ CFG_BAR0,    0x10

.equ CMD_MEM,     0x2          // Memory Space Enable
.equ CMD_BUSM,    0x4          // Bus Master Enable
.equ BAR0_BASE,   0x10000000   // address we assign BAR0 (in the MMIO32 window)

// --- MSI-X ---
.equ CFG_BAR1,    0x14
.equ CFG_CAP_PTR, 0x34
.equ MSIX_WIN,    0x0A005000   // BAR1 fixed base (read back, not assigned)
.equ MSIXC_CTRL,  0x02         // u16 Message Control, within the cap
.equ MSIX_ENABLE, 0x8000
.equ VEC_MASK,    0x1          // vec_ctrl bit0
.equ TBL_OFF,     0x000
.equ PBA_OFF,     0x800
.equ DOORBELL,    0xC00
.equ MSIX_SPI0,   45
.equ MSIX_SPI1,   46

.equ GICD_BASE,       0x08000000
.equ GICD_ISENABLER1, 0x0104   // SPIs 32..63
.equ GICR_BASE,       0x080A0000
.equ GICR_WAKER,      0x0014
.equ ICC_SRE_EL1,     S3_0_C12_C12_5
.equ ICC_PMR_EL1,     S3_0_C4_C6_0
.equ ICC_IGRPEN1_EL1, S3_0_C12_C12_7
.equ ICC_IAR1_EL1,    S3_0_C12_C12_0
.equ ICC_EOIR1_EL1,   S3_0_C12_C12_1

.section .text.boot.pci
.global _pci_start
_pci_start:
	ldr	x0,=0x40200000 ; mov sp,x0
	ldr	x28,=ECAM
	adr	x19,m_banner ; bl p_puts

	// --- read VendorID/DeviceID (reg 0x00) ---
	ldr	w20,[x28,#CFG_VENDOR]
	cmp	w20,#-1                // 0xFFFFFFFF => no device
	b.eq	noscan
	adr	x19,m_found ; bl p_puts
	mov	w23,w20 ; bl p_word    // vendor(low16)|device(high16)
	adr	x19,m_nl ; bl p_puts

	// --- size BAR0: save, write all-ones, read mask, restore-as-base ---
	ldr	w21,[x28,#CFG_BAR0]    // (initial, =0)
	mov	w1,#-1 ; str w1,[x28,#CFG_BAR0]
	ldr	w22,[x28,#CFG_BAR0]    // size mask
	// size = (~(mask & ~0xF)) + 1   (clear the low type bits first)
	and	w2,w22,#0xFFFFFFF0
	mvn	w2,w2
	add	w2,w2,#1
	adr	x19,m_barsz ; bl p_puts
	mov	w23,w2 ; bl p_word
	adr	x19,m_nl ; bl p_puts

	// --- assign BAR0 base, read back to confirm ---
	ldr	w1,=BAR0_BASE ; str w1,[x28,#CFG_BAR0]
	ldr	w23,[x28,#CFG_BAR0]
	adr	x19,m_barbase ; bl p_puts
	bl	p_word
	adr	x19,m_nl ; bl p_puts

	// --- enable MEM + BUSMASTER in Command ---
	mov	w1,#(CMD_MEM|CMD_BUSM) ; str w1,[x28,#CFG_COMMAND]
	ldr	w23,[x28,#CFG_COMMAND]
	adr	x19,m_cmd ; bl p_puts
	mov	w23,w23 ; bl p_hex2
	adr	x19,m_nl ; bl p_puts

	adr	x19,m_done ; bl p_puts

	// === MSI-X bring-up ===
	bl	pci_gic_setup          // VBAR_EL1 + ICC iface + enable SPI 45 & 46

	// 1. find MSI-X cap: cap_ptr = config[0x34]; walk next-ptr until ID 0x11
	ldrb	w0,[x28,#CFG_CAP_PTR]
8:	cbz	w0,no_msix
	ldrb	w1,[x28,w0,uxtw]       // cap ID at config[cap_ptr]
	cmp	w1,#0x11
	b.eq	found_msix
	add	x2,x28,w0,uxtw ; ldrb w0,[x2,#1] ; b 8b   // follow next-ptr
found_msix:
	mov	x25,x0                 // x25 = cap offset
	add	x24,x28,x25            // x24 = &cap
	adr	x19,m_msix ; bl p_puts
	ldrh	w23,[x24,#MSIXC_CTRL] ; and w23,w23,#0x7FF ; add w23,w23,#1  // table size
	bl	p_hex2 ; adr x19,m_nl ; bl p_puts                           // prints 0x02

	// 2. read BAR1 (fixed base; size-probe is informational, base never moves)
	mov	w1,#-1 ; str w1,[x28,#CFG_BAR1]   // probe (returns size mask)
	ldr	w1,[x28,#CFG_BAR1]               // (size mask read, info only)
	ldr	x26,=MSIX_WIN                    // x26 = table window base (pinned BAR1)

	// 3. program table[0]: addr recorded-only, data=SPI45, UNMASK vector 0
	str	wzr,[x26,#(TBL_OFF+0)]           // addr_lo (recorded only)
	str	wzr,[x26,#(TBL_OFF+4)]           // addr_hi
	mov	w1,#MSIX_SPI0 ; str w1,[x26,#(TBL_OFF+8)]    // data = SPI 45
	str	wzr,[x26,#(TBL_OFF+12)]          // vec_ctrl = 0 (UNMASK vector 0)
	// table[1]: data=SPI46, leave MASKED for the deferred-delivery demo
	str	wzr,[x26,#(TBL_OFF+16)]
	str	wzr,[x26,#(TBL_OFF+20)]
	mov	w1,#MSIX_SPI1 ; str w1,[x26,#(TBL_OFF+24)]
	mov	w1,#VEC_MASK ; str w1,[x26,#(TBL_OFF+28)]    // vec_ctrl = 1 (vector 1 MASKED)

	// 4. enable MSI-X (Message Control bit15)
	ldrh	w1,[x24,#MSIXC_CTRL] ; orr w1,w1,#MSIX_ENABLE ; strh w1,[x24,#MSIXC_CTRL]

	msr	daifclr,#2             // unmask IRQs at PSTATE

	// 5. IMMEDIATE: ring vector 0 (unmasked) -> expect IRQ 45
	adr	x19,m_ring0 ; bl p_puts
	mov	w1,#0 ; str w1,[x26,#DOORBELL]
	mov	x20,#0x400000
9:	subs x20,x20,#1 ; b.ne 9b   // spin so the IRQ prints first

	// 6. MASK PATH: ring vector 1 (MASKED) -> expect PBA bit1, NO IRQ
	adr	x19,m_ring1 ; bl p_puts
	mov	w1,#1 ; str w1,[x26,#DOORBELL]
	ldr	w23,[x26,#PBA_OFF]    // expect 0x2
	adr	x19,m_pba ; bl p_puts ; bl p_hex2 ; adr x19,m_nl ; bl p_puts

	// 7. UNMASK vector 1 -> deferred delivery fires SPI 46 now
	adr	x19,m_unmask ; bl p_puts
	str	wzr,[x26,#(TBL_OFF+28)]  // vec_ctrl[1] = 0 -> flush delivers pending
	mov	x20,#0x400000
10:	subs x20,x20,#1 ; b.ne 10b
	ldr	w23,[x26,#PBA_OFF]    // expect 0x0 (cleared on confirmed delivery)
	adr	x19,m_pba ; bl p_puts ; bl p_hex2 ; adr x19,m_nl ; bl p_puts

	adr	x19,m_msixdone ; bl p_puts
	b	idle
no_msix:
	adr	x19,m_nomsix ; bl p_puts
	b	idle
noscan:
	adr	x19,m_nodev ; bl p_puts
idle:
	mov	x0,#0 ; movk x0,#0xFE11,lsl #16 ; hvc #0   // YIELD
	b	idle

// IRQ handler: full context save (helpers clobber x9/x10/x12/x15; main flow has
// x19/x23 live; also save ELR/SPSR). Print INTID, EOI, return.
msix_irq_handler:
	stp	x0,x1,[sp,#-16]!
	stp	x9,x10,[sp,#-16]!
	stp	x12,x15,[sp,#-16]!
	stp	x18,x19,[sp,#-16]!
	stp	x23,x30,[sp,#-16]!
	mrs	x0,elr_el1 ; mrs x1,spsr_el1 ; stp x0,x1,[sp,#-16]!
	mrs	x18,ICC_IAR1_EL1 ; and w23,w18,#0xFFFFFF   // INTID into callee-stable w23
	adr	x19,m_irq ; bl p_puts                      // (clobbers x9/x10/x12/x15, not x23)
	bl	p_word                                     // print INTID (0x2D / 0x2E)
	adr	x19,m_nl ; bl p_puts
	msr	ICC_EOIR1_EL1,x18
	ldp	x0,x1,[sp],#16 ; msr elr_el1,x0 ; msr spsr_el1,x1
	ldp	x23,x30,[sp],#16
	ldp	x18,x19,[sp],#16
	ldp	x12,x15,[sp],#16
	ldp	x9,x10,[sp],#16
	ldp	x0,x1,[sp],#16
	eret

// set up GICv3 EL1 CPU interface + VBAR; enable SPI 45 & 46 in GICD. clobbers x0,x1.
pci_gic_setup:
	mov	x17,x30
	adr	x0,pci_vectors ; msr vbar_el1,x0 ; isb
	mrs	x0,ICC_SRE_EL1 ; orr x0,x0,#1 ; msr ICC_SRE_EL1,x0 ; isb
	mov	x0,#0xFF ; msr ICC_PMR_EL1,x0
	mov	x0,#1 ; msr ICC_IGRPEN1_EL1,x0 ; isb
	ldr	x0,=GICR_BASE ; ldr w1,[x0,#GICR_WAKER] ; and w1,w1,#~2 ; str w1,[x0,#GICR_WAKER]
	ldr	x0,=GICD_BASE
	mov	w1,#((1<<(MSIX_SPI0-32))|(1<<(MSIX_SPI1-32)))   // bits 13,14 -> 0x6000
	str	w1,[x0,#GICD_ISENABLER1]
	mov	x30,x17 ; ret

.balign 2048
pci_vectors:
	.balign 128 ; eret                 // 0 cur-EL-SP0 sync
	.balign 128 ; eret                 // 1 irq
	.balign 128 ; eret                 // 2 fiq
	.balign 128 ; eret                 // 3 serror
	.balign 128 ; eret                 // 4 cur-EL-SPx sync
	.balign 128 ; b msix_irq_handler   // 5 cur-EL-SPx IRQ  <- MSI-X SPI lands here
	.balign 128 ; eret                 // 6 fiq
	.balign 128 ; eret                 // 7 serror
	.balign 128 ; eret                 // 8..15 lower-EL (unused)
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret

// helpers
p_puts:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
1:	ldrb w11,[x19],#1 ; cbz w11,2f
3:	ldr w12,[x10] ; tbnz w12,#5,3b ; str w11,[x9] ; b 1b
2:	ret
p_hex2:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
	lsr	w15,w23,#4 ; and w15,w15,#0xF ; cmp w15,#10 ; b.lt 4f ; add w15,w15,#('A'-10) ; b 5f
4:	add	w15,w15,#'0'
5:	ldr	w12,[x10] ; tbnz w12,#5,5b ; str w15,[x9]
	and	w15,w23,#0xF ; cmp w15,#10 ; b.lt 6f ; add w15,w15,#('A'-10) ; b 7f
6:	add	w15,w15,#'0'
7:	ldr	w12,[x10] ; tbnz w12,#5,7b ; str w15,[x9] ; ret
p_word:                        // print w23 as 8 hex digits
	mov	x16,x30 ; mov w13,w23
	lsr	w23,w13,#24 ; bl p_hex2
	lsr	w23,w13,#16 ; bl p_hex2
	lsr	w23,w13,#8  ; bl p_hex2
	mov	w23,w13     ; bl p_hex2
	mov	x30,x16 ; ret

.section .rodata.pci
m_banner:  .asciz "\r\n[pciclient] enumerating virtual PCI bus (ECAM)\r\n"
m_found:   .asciz "[pciclient] device @ 00:00.0 vendor|device = 0x"
m_barsz:   .asciz "[pciclient] BAR0 size = 0x"
m_barbase: .asciz "[pciclient] BAR0 assigned base = 0x"
m_cmd:     .asciz "[pciclient] command reg (MEM|BUSM) = 0x"
m_done:    .asciz "[pciclient] PCI device configured\r\n"
m_nodev:   .asciz "[pciclient] no device found\r\n"
m_msix:    .asciz "[pciclient] MSI-X cap found, table size = 0x"
m_ring0:   .asciz "[pciclient] ring doorbell vector 0 (unmasked)\r\n"
m_irq:     .asciz "[pciclient] got MSI-X IRQ, INTID = 0x"
m_ring1:   .asciz "[pciclient] ring doorbell vector 1 (MASKED, expect no IRQ)\r\n"
m_pba:     .asciz "[pciclient] PBA = 0x"
m_unmask:  .asciz "[pciclient] unmasking vector 1 -> deferred delivery\r\n"
m_msixdone:.asciz "[pciclient] MSI-X test complete\r\n"
m_nomsix:  .asciz "[pciclient] no MSI-X capability!\r\n"
m_nl:      .asciz "\r\n"
