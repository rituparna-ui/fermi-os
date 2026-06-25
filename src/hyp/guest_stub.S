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
