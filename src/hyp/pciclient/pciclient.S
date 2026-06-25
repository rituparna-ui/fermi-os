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
	b	idle
noscan:
	adr	x19,m_nodev ; bl p_puts
idle:
	mov	x0,#0 ; movk x0,#0xFE11,lsl #16 ; hvc #0   // YIELD
	b	idle

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
m_nl:      .asciz "\r\n"
