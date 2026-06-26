// smpguest: a 2-vCPU SMP guest demonstrating GICv3 inter-processor SGIs AND
// full PSCI CPU hotplug (online -> offline -> online). The primary (Aff0=0)
// brings up the secondary (Aff0=1) via PSCI CPU_ON, then the two ping-pong a
// GICv3 SGI that the hypervisor traps (ICH_HCR_EL2.TC) and software-routes to
// the sibling vCPU. Both vCPUs run the SAME image at IPA 0x40000000 on a SHARED
// stage-2 (same VTTBR/VMID); MPIDR_EL1 (= per-vCPU VMPIDR_EL2) distinguishes
// them. They are time-sliced onto the one physical CPU by the EL2 scheduler, so
// each bounce of the ping-pong forces a world switch between the siblings.
//
// Flow (a self-sustaining hotplug cycle):
//   primary (entered at reset, Aff0=0):
//     set SP, banner, set up EL1 IRQ vector + GICv3 CPU interface, enable SGI 0,
//     PSCI CPU_ON(aff=1, entry=_secondary_entry, ctx=0x5EC0), unmask IRQ, send
//     the FIRST SGI 0 to CPU1, then run super_loop: poll PSCI AFFINITY_INFO(CPU1)
//     (wfi-throttled, woken by inbound ping SGIs); when CPU1 reads OFF, count a
//     hotplug cycle, CPU_ON it again, and re-kick the ping-pong.
//   secondary (entered by the hyp at _secondary_entry, x0=context_id; re-entered
//     here on every re-CPU_ON): verify MPIDR Aff0=1, print Aff0 + ctx, reset the
//     per-online ping counter, set up GIC iface, unmask IRQ, idle.
//   shared IRQ handler (both CPUs): ack the SGI, print "CPUn got IPI #k", bump a
//     shared display counter. The PRIMARY then SGIs CPU1 back. The SECONDARY
//     bumps its per-online counter and, after OFF_AT pings, calls PSCI CPU_OFF
//     to power ITSELF down (it does not return; a later CPU_ON re-enters
//     _secondary_entry) — otherwise it SGIs CPU0 back.
//
// _smp_start MUST be the first byte of the image: the hyp erets to IPA
// 0x40000000. Links flat at 0x40000000; embedded via smpguest_blob.S.

.equ UART_DR, 0x09000000
.equ UART_FR, 0x09000018

.equ GICD_BASE, 0x08000000
.equ GICD_ISENABLER0, 0x0100
.equ GICR_BASE, 0x080A0000
.equ GICR_WAKER, 0x0014

// ICC_* system register encodings (binutils accepts these Sn_n_Cn_Cn_n forms)
.equ ICC_SRE_EL1,    S3_0_C12_C12_5
.equ ICC_PMR_EL1,    S3_0_C4_C6_0
.equ ICC_IGRPEN1_EL1,S3_0_C12_C12_7
.equ ICC_IAR1_EL1,   S3_0_C12_C12_0
.equ ICC_EOIR1_EL1,  S3_0_C12_C12_1
.equ ICC_SGI1R_EL1,  S3_0_C12_C11_5

.equ SGI_INTID, 0
.equ AP_AFF,    1              // secondary Aff0
.equ CTX_ID,    0x5EC0
.equ OFF_AT,    3              // secondary offlines after this many pings/online window

.section .text.boot.smp
.global _smp_start
_smp_start:
	// Distinguish primary vs secondary by Aff0 of MPIDR_EL1. At reset only the
	// primary (Aff0=0) runs here; the hyp enters the AP at _secondary_entry.
	mrs	x9, mpidr_el1
	and	x9, x9, #0xFF
	cbnz	x9, _secondary_entry

// ---------------- primary (CPU0) ----------------
primary:
	ldr	x0,=0x40200000 ; mov sp,x0
	adr	x19,m_pbanner ; bl s_puts
	bl	gic_setup_and_vectors

	// PSCI CPU_ON(secondary): x0=fn 0xC4000003, x1=target aff, x2=entry, x3=ctx
	mov	x0,#0
	movk	x0,#0xC400,lsl #16 ; movk x0,#0x0003
	mov	x1,#AP_AFF
	ldr	x2,=_secondary_entry
	ldr	x3,=CTX_ID
	hvc	#0
	adr	x19,m_cpuon ; bl s_puts
	mov	w23,w0 ; bl s_hex2        // PSCI return (00 = SUCCESS)
	adr	x19,m_nl ; bl s_puts

	msr	daifclr,#2                // unmask IRQs

	// kick off the ping-pong: send the first SGI 0 to CPU1 (Aff0=1 => bit1).
	// TargetList=1<<1, INTID 0 (bits[27:24]=0), Aff1..3=0, IRM=0 => x0 = 2.
	mov	x0,#(1<<AP_AFF)
	msr	ICC_SGI1R_EL1, x0
	isb

// primary supervisor: poll PSCI AFFINITY_INFO(CPU1) on a throttle, yielding the
// CPU between polls (HVC_FERMI_YIELD) so other VMs run and we stay live. We do
// NOT wfi here: CPU1 stops sending ping SGIs on the very ping where it offlines,
// so a wfi waiting for that SGI would never wake. When CPU1 reports OFF, count a
// hotplug cycle, CPU_ON it again, and re-kick the ping-pong — a self-sustaining
// online->offline->online cycle. Ping-pong IRQs are still serviced concurrently
// (IRQs stay unmasked), the handler's throttle keeps them human-visible.
super_loop:
20:	mov	x0,#0
	movk	x0,#0xC400,lsl #16 ; movk x0,#0x0004   // x0 = 0xC4000004 AFFINITY_INFO
	mov	x1,#AP_AFF                              // target Aff0 = 1
	hvc	#0
	cbnz	x0, off_seen                           // x0==1 (OFF) -> re-online it
	// throttle, then yield to other VMs and poll again
	mov	x20,#0x800000
21:	subs x20,x20,#1 ; b.ne 21b
	mov	x0,#0 ; movk x0,#0xFE11,lsl #16          // HVC_FERMI_YIELD
	hvc	#0
	b	20b

off_seen:
	adr	x19,m_off ; bl s_puts
	ldr	x11,=cycle_count ; ldr w23,[x11] ; add w23,w23,#1 ; str w23,[x11]
	adr	x19,m_cyc ; bl s_puts ; bl s_hex2 ; adr x19,m_nl ; bl s_puts
	// CPU_ON the secondary again (same entry/ctx as boot)
	mov	x0,#0
	movk	x0,#0xC400,lsl #16 ; movk x0,#0x0003   // CPU_ON
	mov	x1,#AP_AFF
	ldr	x2,=_secondary_entry
	ldr	x3,=CTX_ID
	hvc	#0
	adr	x19,m_reon ; bl s_puts ; mov w23,w0 ; bl s_hex2 ; adr x19,m_nl ; bl s_puts
	// re-kick the ping-pong with a fresh SGI to CPU1
	mov	x0,#(1<<AP_AFF)
	msr	ICC_SGI1R_EL1, x0
	isb
	b	super_loop

// ---------------- secondary (CPU1), entered by the hyp via PSCI CPU_ON -------
.global _secondary_entry
_secondary_entry:
	mov	x21,x0                    // x0 = context_id (PSCI contract)
	ldr	x0,=0x40280000 ; mov sp,x0   // distinct stack from the primary
	adr	x19,m_sbanner ; bl s_puts
	mrs	x23,mpidr_el1 ; and w23,w23,#0xFF ; bl s_hex2   // our Aff0 (expect 01)
	adr	x19,m_ctx ; bl s_puts
	mov	w23,w21 ; bl s_hex2       // context_id low byte (expect C0)
	adr	x19,m_nl ; bl s_puts
	ldr	x11,=pings_since_on ; str wzr,[x11]   // fresh per-online window
	bl	gic_setup_and_vectors
	msr	daifclr,#2                // unmask IRQs; the rest is IRQ-driven
2:	wfi
	b	2b

// shared IRQ handler (both CPUs): ack SGI, print, bump shared counter, throttle,
// then SGI the OTHER CPU back. Runs with IRQs masked (entry masks I); EOI+eret
// restores. Clobbers caller-saved regs only (no GP state shared with main path
// beyond what the world-switch preserves).
sgi_irq_handler:
	mrs	x18, ICC_IAR1_EL1         // ack; x18 = INTID (callee-stable across UART)
	and	x10, x18, #0xFFFFFF
	cmp	x10, #SGI_INTID
	b.ne	9f

	// who am I? Aff0 in w24 (callee-stable across the UART helpers)
	mrs	x24, mpidr_el1 ; and w24, w24, #0xFF

	// bump the SHARED ping counter in RAM (siblings share stage-2). Plain
	// load/add/store is safe: the two vCPUs are time-sliced, never concurrent.
	ldr	x11,=ping_count ; ldr w25,[x11] ; add w25,w25,#1 ; str w25,[x11]

	adr	x19,m_cpu ; bl s_puts_irq
	mov	w23,w24 ; bl s_hex2_irq    // which CPU took the IPI
	adr	x19,m_ipi ; bl s_puts_irq
	mov	w23,w25 ; bl s_hex2_irq    // shared ping count
	adr	x19,m_nl ; bl s_puts_irq

	// The PRIMARY (Aff0=0) always just bounces the ping back. The SECONDARY
	// (Aff0!=0) bumps its per-online counter and, after OFF_AT pings, powers
	// ITSELF down via PSCI CPU_OFF.
	cbz	w24, do_pingback           // primary -> just pong
	ldr	x11,=pings_since_on ; ldr w26,[x11] ; add w26,w26,#1 ; str w26,[x11]
	cmp	w26,#OFF_AT
	b.lt	do_pingback
	// --- secondary offlines itself via PSCI CPU_OFF ---
	msr	ICC_EOIR1_EL1, x18         // EOI the in-flight SGI (we won't reach 9:)
	adr	x19,m_soff ; bl s_puts_irq
	mov	x0,#0
	movk	x0,#0x8400,lsl #16 ; movk x0,#0x0002   // x0 = 0x84000002 CPU_OFF
	hvc	#0
	// Returns ONLY on failure (DENIED). On success the hyp switched away and a
	// later CPU_ON re-enters _secondary_entry, not here.
	adr	x19,m_odn ; bl s_puts_irq
	b	do_pingback                // DENIED -> rejoin the ping-pong

do_pingback:
	// throttle so the bounce is human-visible, then poke the OTHER CPU.
	mov	x12,#0x400000
3:	subs x12,x12,#1 ; b.ne 3b
	mov	w13,#1 ; eor w13,w13,w24    // other Aff0 = 1 - self (0<->1)
	mov	x0,#1 ; lsl x0,x0,x13       // TargetList = 1 << other_aff0
	msr	ICC_SGI1R_EL1, x0
	isb
9:	msr	ICC_EOIR1_EL1, x18
	eret

// set up GICv3 CPU interface + EL1 vector base; enable SGI INTID 0. The vector
// table is at smp_vectors (2 KiB aligned, below). clobbers x0,x1.
gic_setup_and_vectors:
	mov	x17, x30
	adr	x0, smp_vectors ; msr vbar_el1, x0 ; isb
	mrs	x0, ICC_SRE_EL1 ; orr x0,x0,#1 ; msr ICC_SRE_EL1, x0 ; isb
	mov	x0,#0xFF ; msr ICC_PMR_EL1, x0       // unmask all priorities
	mov	x0,#1 ; msr ICC_IGRPEN1_EL1, x0 ; isb // enable Group1 IRQs
	// wake redistributor (our emulated GICR_WAKER reads 0 anyway)
	ldr	x0,=GICR_BASE ; ldr w1,[x0,#GICR_WAKER] ; and w1,w1,#~2 ; str w1,[x0,#GICR_WAKER]
	// enable SGI INTID 0 in GICD ISENABLER0
	ldr	x0,=GICD_BASE ; mov w1,#(1<<SGI_INTID) ; str w1,[x0,#GICD_ISENABLER0]
	mov	x30, x17
	ret

// --- UART helpers (clobber x9,x10,x11,x12,x15) + x30-saving IRQ wrappers ---
s_puts:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
4:	ldrb w11,[x19],#1 ; cbz w11,5f
6:	ldr w12,[x10] ; tbnz w12,#5,6b ; str w11,[x9] ; b 4b
5:	ret
s_hex2:
	ldr	x9,=UART_DR ; ldr x10,=UART_FR
	lsr	w15,w23,#4 ; and w15,w15,#0xF ; cmp w15,#10 ; b.lt 7f ; add w15,w15,#('A'-10) ; b 8f
7:	add	w15,w15,#'0'
8:	ldr	w12,[x10] ; tbnz w12,#5,8b ; str w15,[x9]
	and	w15,w23,#0xF ; cmp w15,#10 ; b.lt 10f ; add w15,w15,#('A'-10) ; b 11f
10:	add	w15,w15,#'0'
11:	ldr	w12,[x10] ; tbnz w12,#5,11b ; str w15,[x9] ; ret
s_puts_irq:
	mov	x16,x30 ; bl s_puts ; mov x30,x16 ; ret
s_hex2_irq:
	mov	x16,x30 ; bl s_hex2 ; mov x30,x16 ; ret

// EL1 vector table (VBAR_EL1). Only the current-EL-SPx IRQ entry is interesting.
.balign 2048
smp_vectors:
	.balign 128 ; eret              // 0 cur-EL-SP0 sync
	.balign 128 ; eret              // 1 irq
	.balign 128 ; eret              // 2 fiq
	.balign 128 ; eret              // 3 serror
	.balign 128 ; eret              // 4 cur-EL-SPx sync
	.balign 128 ; b sgi_irq_handler // 5 cur-EL-SPx IRQ  <- SGI lands here
	.balign 128 ; eret              // 6 fiq
	.balign 128 ; eret              // 7 serror
	.balign 128 ; eret              // 8..15 lower-EL (unused)
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret
	.balign 128 ; eret

.section .data.smp
.balign 8
ping_count:     .word 0      // total pings (display only)
pings_since_on: .word 0      // reset at _secondary_entry; drives the CPU_OFF trigger
cycle_count:    .word 0      // completed offline->online hotplug cycles

.section .rodata.smp
m_pbanner: .asciz "\r\n[smpguest] primary (CPU0) up, bringing up CPU1 via PSCI CPU_ON\r\n"
m_cpuon:   .asciz "[smpguest] PSCI CPU_ON returned 0x"
m_sbanner: .asciz "[smpguest] secondary (CPU1) running, Aff0=0x"
m_ctx:     .asciz " ctx=0x"
m_cpu:     .asciz "[smpguest] CPU"
m_ipi:     .asciz " got IPI, ping #0x"
m_soff:    .asciz "[smpguest] CPU1 calling PSCI CPU_OFF\r\n"
m_off:     .asciz "[smpguest] primary observed CPU1 OFF via AFFINITY_INFO\r\n"
m_reon:    .asciz "[smpguest] primary re-CPU_ON CPU1, ret=0x"
m_cyc:     .asciz "[smpguest] === hotplug cycle 0x"
m_odn:     .asciz "[smpguest] CPU_OFF DENIED, rejoining ping-pong\r\n"
m_nl:      .asciz "\r\n"
