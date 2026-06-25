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
