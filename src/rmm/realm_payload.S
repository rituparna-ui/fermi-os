/* ---------------------------------------------------------------------------
 * realm_payload.S — a minimal Realm payload (R4a)
 *
 * Runs at EL1 under the realm's private stage-2 (RTT) with the stage-1 MMU
 * OFF, so it executes at its mapped IPA. Position-independent: only a couple
 * of register ops and an HVC. It proves the world switch by computing a value
 * in x0 (0x42) and then exiting to the RMM via `hvc`, which the monitor sees
 * as a realm exit and reports back to the Normal-world host.
 *
 * The bytes here are linked into the kernel image; the RMM copies them into a
 * realm DATA granule (RMI_DATA_CREATE) and maps that page at the realm's entry
 * IPA before RMI_REC_ENTER.
 * --------------------------------------------------------------------------- */

.section .text.realm, "ax"
.global realm_payload
.global realm_payload_end

realm_payload:
	mov	x0, #0x41
	add	x0, x0, #1        // x0 = 0x42: evidence the realm actually ran
	hvc	#0                // trap to EL2 -> realm exit back to the host
1:
	wfi                       // (not re-entered in R4a)
	b	1b
realm_payload_end:
