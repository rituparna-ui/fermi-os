// smpguest: a 2-vCPU SMP guest. The primary (Aff0=0) brings up the secondary
// (Aff0=1) via PSCI CPU_ON, then the two ping-pong a GICv3 SGI (inter-processor
// interrupt) that the hypervisor traps (ICH_HCR_EL2.TC) and software-routes to
// the sibling vCPU. Both vCPUs run the SAME image at IPA 0x40000000 on a SHARED
// stage-2 (same VTTBR/VMID); MPIDR_EL1 (= per-vCPU VMPIDR_EL2) distinguishes
// them. They are time-sliced onto the one physical CPU by the EL2 scheduler, so
// each bounce of the ping-pong forces a world switch between the siblings.
//
// Flow:
//   primary (entered at reset, Aff0=0):
//     set SP, banner, set up EL1 IRQ vector + GICv3 CPU interface, enable SGI 0,
//     PSCI CPU_ON(target aff=1, entry=_secondary_entry, ctx=0x5EC0), unmask IRQ,
//     send the FIRST SGI 0 to CPU1, then idle in WFI.
//   secondary (entered by the hyp at _secondary_entry, x0=context_id):
//     verify MPIDR Aff0=1, print Aff0 + ctx, set up GIC iface, unmask IRQ, idle.
//   shared IRQ handler (both CPUs): ack the SGI, print "CPUn got IPI #k", bump a
//     SHARED counter (the siblings share RAM), throttle, then SGI the OTHER CPU
//     back — an endless ping-pong proving both routing directions + shared mem.
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
1:	wfi
	b	1b

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
ping_count: .word 0

.section .rodata.smp
m_pbanner: .asciz "\r\n[smpguest] primary (CPU0) up, bringing up CPU1 via PSCI CPU_ON\r\n"
m_cpuon:   .asciz "[smpguest] PSCI CPU_ON returned 0x"
m_sbanner: .asciz "[smpguest] secondary (CPU1) running, Aff0=0x"
m_ctx:     .asciz " ctx=0x"
m_cpu:     .asciz "[smpguest] CPU"
m_ipi:     .asciz " got IPI, ping #0x"
m_nl:      .asciz "\r\n"
