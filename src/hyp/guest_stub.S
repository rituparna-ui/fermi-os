// FermiOS hypervisor — trivial EL1 guest stub (milestone 3 smoke test).
//
// Runs at EL1 under stage-2 translation, entered by the world switch at guest
// IPA 0x40000000. It is fully position-independent (no literal pool / no
// absolute relocations) so the host can copy these raw bytes into guest RAM
// and enter at the start. It:
//   1. writes a known 64-bit marker to guest IPA 0x40000800 (proves the guest
//      can touch RAM through its own EL1 view + stage-2),
//   2. loads a sentinel into x2 (proves GPRs survive the world switch),
//   3. executes `hvc #0` (proves the guest->EL2 trap path: EC=0x16),
//   4. spins in wfe in case it is ever re-entered.
//
// The host reads back the marker and x2 after the exit to confirm success.

.equ GUEST_MARKER_LO, 0xBABE
.equ GUEST_MARKER_HI, 0xFEED		// full marker = 0xFEEDFACECAFEBABE
.equ GUEST_X2_SENTINEL, 0xBEEF

.section .text.guest, "ax"
.balign 16
.global guest_stub_start
guest_stub_start:
	// x1 = marker address (IPA 0x40000800), built with movz/movk.
	movz	x1, #0x0000
	movk	x1, #0x4000, lsl #16	// x1 = 0x40000000
	add	x1, x1, #0x800		// x1 = 0x40000800

	// x0 = 0xFEEDFACECAFEBABE
	movz	x0, #0xBABE
	movk	x0, #0xCAFE, lsl #16
	movk	x0, #0xFACE, lsl #32
	movk	x0, #0xFEED, lsl #48
	str	x0, [x1]
	dsb	ish

	// x2 = sentinel the host checks on exit.
	movz	x2, #GUEST_X2_SENTINEL

	// Trap to the hypervisor.
	hvc	#0

1:	wfe
	b	1b

.global guest_stub_end
guest_stub_end:

// ---------------------------------------------------------------------------
// Spinning EL1 guest (milestone 4 time-slice demo). Increments x10 forever and
// never voluntarily exits, so the ONLY way control returns to the hypervisor is
// an EL2-routed interrupt (HCR_EL2.IMO=1 + the EL2 physical timer). The host
// reads x10 across slices to prove the guest both ran and resumed.
.section .text.guest, "ax"
.balign 16
.global guest_spin_start
guest_spin_start:
	movz	x10, #0
1:	add	x10, x10, #1
	b	1b
.global guest_spin_end
guest_spin_end:

// ---------------------------------------------------------------------------
// Heartbeat EL1 guest (milestone 9 multi-guest demo). Position-independent.
// On entry the hypervisor seeds x0 = this guest's ASCII id char ('A','B',...).
// Each "beat": increment a per-guest counter (x10, lives in the per-vCPU GP
// context across world switches), write the id char to the PL011 UART data
// register, store the counter to the guest's own RAM at 0x40000800, then HVC to
// voluntarily yield back to the hypervisor scheduler. x0 (the id) is preserved
// across the loop to prove per-vCPU register isolation.
.section .text.guest, "ax"
.balign 16
.global guest_hb_start
guest_hb_start:
	// x9 = UART data register (IPA, straight-through mapped).
	movz	x9, #0x0000
	movk	x9, #0x0900, lsl #16	// x9 = 0x09000000
	// x11 = counter store address (guest RAM base + 0x800).
	movz	x11, #0x0000
	movk	x11, #0x4000, lsl #16
	add	x11, x11, #0x800
	movz	x10, #0			// per-guest beat counter
1:	add	x10, x10, #1
	str	w0, [x9]		// print our id char
	str	x10, [x11]		// record counter in our own RAM
	hvc	#0			// yield to the hypervisor scheduler
	b	1b
.global guest_hb_end
guest_hb_end:

// ---------------------------------------------------------------------------
// PSCI test EL1 guest (milestone 11). Counts beats in x10; on the FIRST beat it
// records "ran before reset" by storing 0x1111 to RAM, then issues PSCI
// SYSTEM_RESET (hvc with x0=0x84000009). The hypervisor should warm-reset it,
// re-entering at the top with x10 cleared — so after reset x10 climbs from 0
// again. The host watches x10 reset to prove the warm reset happened.
// On entry the hypervisor seeds x0 = a "life" tag: 0 for the first boot, 1 for
// the post-reset boot (the host sets x0 when it warm-resets). The guest stores
// its beat counter to RAM each beat. On the FIRST life (x0==0) it requests PSCI
// SYSTEM_RESET on beat 1; on the SECOND life (x0!=0) it just counts up, so the
// host can see the counter climb past 1 only AFTER the reset.
.section .text.guest, "ax"
.balign 16
.global guest_psci_start
guest_psci_start:
	mov	x12, x0			// x12 = life tag (preserved across beats)
	movz	x11, #0x0000
	movk	x11, #0x4000, lsl #16
	add	x11, x11, #0x800	// RAM marker addr
	movz	x10, #0			// beat counter
1:	add	x10, x10, #1
	str	x10, [x11]
	// First life only, first beat only: request PSCI SYSTEM_RESET.
	cbnz	x12, 2f			// second life -> never reset
	cmp	x10, #1
	b.ne	2f
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16	// x0 = 0x84000009 (SYSTEM_RESET)
	hvc	#0
2:	hvc	#0			// yield to the hypervisor scheduler
	b	1b
.global guest_psci_end
guest_psci_end:

// ---------------------------------------------------------------------------
// Inter-VM shared-memory + doorbell guests (milestone 15). The hypervisor maps
// one host page into BOTH guests' stage-2 at IPA 0x50000000, and provides a
// doorbell hypercall (HVC x0=0xF000_0001) that injects an SPI into the peer VM.
//
// Shared page layout (8-byte slots at 0x50000000):
//   +0x00  producer counter (producer writes, consumer reads)
//   +0x08  doorbell sequence (producer bumps before ringing)
//
// guest_shm: x0 = role (0 = producer, 1 = consumer), x9 = UART base.
// Producer: bump counter in shared mem, ring the doorbell, yield.
// Consumer: read the shared counter, print "C<low-hex-digit>", yield.
.section .text.guest, "ax"
.balign 16
.global guest_shm_start
guest_shm_start:
	mov	x12, x0			// role
	movz	x9, #0x0000
	movk	x9, #0x0900, lsl #16	// UART base 0x09000000
	movz	x11, #0x0000
	movk	x11, #0x5000, lsl #16	// shared page IPA 0x50000000
	cbnz	x12, shm_consumer

shm_producer:
	ldr	x10, [x11]		// counter
	add	x10, x10, #1
	str	x10, [x11]		// publish new counter
	ldr	x13, [x11, #8]
	add	x13, x13, #1
	str	x13, [x11, #8]		// bump doorbell seq
	dsb	ish
	mov	w14, #'P'
	str	w14, [x9]		// show the producer ran
	movz	x0, #0x0001
	movk	x0, #0xF000, lsl #16	// x0 = 0xF0000001 (doorbell hypercall)
	hvc	#0
	hvc	#0			// yield slice
	b	shm_producer

shm_consumer:
	ldr	x10, [x11]		// read shared counter
	mov	w14, #'C'
	str	w14, [x9]
	and	x15, x10, #0xF		// low nibble -> hex digit
	cmp	x15, #10
	b.lt	1f
	add	x15, x15, #('a' - 10)
	b	2f
1:	add	x15, x15, #'0'
2:	str	w15, [x9]		// print the counter's low digit
	hvc	#0			// yield slice
	b	shm_consumer
.global guest_shm_end
guest_shm_end:

// ---------------------------------------------------------------------------
// Interrupt-driven doorbell guests (milestone 16). Same shared page at IPA
// 0x50000000. The PRODUCER (x0=0) writes the counter + rings the doorbell HVC.
// The CONSUMER (x0=1) installs a minimal EL1 IRQ vector table, enables the
// GICv3 CPU interface, unmasks IRQs and WFIs; when the hypervisor injects the
// doorbell SPI (INTID 40) the consumer's IRQ handler acks it, reads the shared
// counter, prints 'D<hex>', and EOIs. This proves the full inter-VM virtual-IRQ
// delivery path (peer-injected SPI, not the timer).
//
// VBAR_EL1 is set via PC-relative `adr` to the (2 KiB-aligned) vector table in
// this same blob, so no fixed IPA / precise blob layout is required.
.section .text.guest, "ax"
.balign 16
.global guest_db_start
guest_db_start:
	mov	x12, x0			// role
	movz	x9,  #0x0000
	movk	x9,  #0x0900, lsl #16	// UART
	movz	x11, #0x0000
	movk	x11, #0x5000, lsl #16	// shared page
	cbnz	x12, db_consumer

db_producer:
	ldr	x10, [x11]
	add	x10, x10, #1
	str	x10, [x11]		// publish counter
	ldr	x13, [x11, #8]
	add	x13, x13, #1
	str	x13, [x11, #8]		// doorbell seq
	dsb	ish
	mov	w14, #'P'
	str	w14, [x9]
	movz	x0, #0x0001
	movk	x0, #0xF000, lsl #16	// doorbell hypercall 0xF0000001
	hvc	#0
	hvc	#0			// yield
	b	db_producer

db_consumer:
	adr	x0, db_vectors		// PC-relative -> runtime IPA of the table
	msr	vbar_el1, x0
	isb
	mrs	x0, S3_0_C12_C12_5	// ICC_SRE_EL1
	orr	x0, x0, #1		// SRE
	msr	S3_0_C12_C12_5, x0
	isb
	mov	x0, #0xFF
	msr	S3_0_C4_C6_0, x0	// ICC_PMR_EL1 = 0xFF (allow all priorities)
	mov	x0, #1
	msr	S3_0_C12_C12_7, x0	// ICC_IGRPEN1_EL1 = 1 (enable Group1)
	isb
	msr	daifclr, #2		// unmask IRQ
db_idle:
	wfi
	b	db_idle
.global guest_db_end
guest_db_end:

// EL1 vector table for the consumer. 16 entries x 0x80B. The vCPU runs at EL1h
// (SP_ELx), so the IRQ we care about is "Current EL with SP_ELx, IRQ" = entry
// index 5, at offset 0x280.
.balign 0x800
db_vectors:
	.balign 0x80
	b	.			// 0x000 cur-EL SP0  sync
	.balign 0x80
	b	.			// 0x080 cur-EL SP0  irq
	.balign 0x80
	b	.			// 0x100 cur-EL SP0  fiq
	.balign 0x80
	b	.			// 0x180 cur-EL SP0  serror
	.balign 0x80
	b	.			// 0x200 cur-EL SPx  sync
	.balign 0x80
				// 0x280 cur-EL SPx  IRQ  <-- the doorbell lands here
	mov	x9, xzr
	movz	x9, #0x0000
	movk	x9, #0x0900, lsl #16	// UART
	mrs	x8, S3_0_C12_C12_0	// ICC_IAR1_EL1 (ack) -> x8 = vINTID
	movz	x11, #0x0000
	movk	x11, #0x5000, lsl #16
	ldr	x10, [x11]		// read shared counter
	ldr	x16, [x11, #16]		// shared[2] = "handled" count
	add	x16, x16, #1
	str	x16, [x11, #16]		// record that the handler ran
	dsb	ish
	mov	w14, #'D'
	str	w14, [x9]
	and	x15, x10, #0xF
	cmp	x15, #10
	b.lt	3f
	add	x15, x15, #('a' - 10)
	b	4f
3:	add	x15, x15, #'0'
4:	str	w15, [x9]
	msr	S3_0_C12_C12_1, x8	// ICC_EOIR1_EL1 (EOI the acked INTID)
	eret
	.balign 0x80
	b	.			// 0x300 cur-EL SPx  fiq
	.balign 0x80
	b	.			// 0x380 cur-EL SPx  serror
.global guest_db_vectors_end
guest_db_vectors_end:

// ---------------------------------------------------------------------------
// Interactive echo EL1 guest (milestone 12). A tiny PL011 "shell": prints a
// banner + prompt, then loops reading a char (poll FR.RXFE, read DR) and
// echoing it back, re-printing the prompt after a newline. Proves the vuart RX
// path (host console input -> guest) end to end, without the full FermiOS boot
// (no PCI scan, no MMU) so it reaches input handling immediately.
//
// Strings are inlined in .text.guest (with the code) so the single blob copy
// includes them and the PC-relative `adr` resolves at the guest load address.
.section .text.guest, "ax"
.balign 16
.global guest_echo_start
guest_echo_start:
	// x9 = UART base (IPA 0x09000000)
	movz	x9, #0x0000
	movk	x9, #0x0900, lsl #16
	adr	x20, gi_banner
	bl	gi_puts
gi_prompt_loop:
	adr	x20, gi_prompt
	bl	gi_puts
gi_readloop:
	// wait for RX: poll FR (off 0x18) bit4 RXFE == 0
1:	ldr	w10, [x9, #0x18]
	tbnz	w10, #4, 1b		// RXFE set -> empty, keep polling
	ldr	w11, [x9, #0x00]	// read DR
	str	w11, [x9, #0x00]	// echo it back
	cmp	w11, #'\n'
	b.eq	gi_prompt_loop
	cmp	w11, #'\r'
	b.eq	gi_prompt_loop
	b	gi_readloop

// gi_puts: write the NUL-terminated string at x20 to the UART (x9=base).
gi_puts:
2:	ldrb	w11, [x20], #1
	cbz	w11, 3f
	str	w11, [x9, #0x00]
	b	2b
3:	ret

.balign 4
gi_banner: .asciz "vguest: type and I echo it back\n"
.balign 4
gi_prompt: .asciz "vguest> "
.balign 16
.global guest_echo_end
guest_echo_end:
