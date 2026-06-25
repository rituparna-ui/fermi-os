// dom0: a privileged "control domain" guest (EL1). The hypervisor sets its
// `privileged` flag, so it may issue the VMCTL management hypercall against the
// other VMs — the Xen-dom0 / libvirt-virsh model: enumerate VMs, query their
// state + run-count, and pause / resume / reset them at runtime.
//
// It runs a one-shot management script then idles (yielding), so the demo
// output is deterministic:
//   1. VMCTL_COUNT          -> print "vms=N"
//   2. for each VM: VMCTL_STATE + VMCTL_RUNS -> print id/state/runs
//   3. VMCTL_STOP  VM 1     -> pause the heartbeat guest
//   4. (idle a while; VM1 produces no heartbeats while paused)
//   5. VMCTL_START VM 1     -> resume it
//   6. VMCTL_RESET VM 2     -> warm-reset the IPC producer
//   7. idle forever (yield)
//
// Built flat + embedded (dom0_blob.S); links at IPA 0x40000000 like the others.

.equ UART_DR,  0x09000000
.equ UART_FR,  0x09000018

.equ HVC_YIELD, 0xFE110000
// VMCTL ops (must match vm.h)
.equ VMCTL_COUNT, 0
.equ VMCTL_STATE, 1
.equ VMCTL_RUNS,  2
.equ VMCTL_RESET, 3
.equ VMCTL_STOP,  4
.equ VMCTL_START, 5
.equ VMCTL_STAT,  6
.equ VMCTL_WEIGHT, 7
.equ VMCTL_CPUTIME, 8
.equ VMCTL_SNAPSHOT, 9
.equ VMCTL_RESTORE, 10

.section .text.boot.dom0
.global _dom0_start
_dom0_start:
	ldr	x0, =0x40200000
	mov	sp, x0

	adr	x19, m_hdr
	bl	d_puts

	// --- 1. VMCTL_COUNT ---
	bl	vmctl_count           // -> x0 = N
	mov	x22, x0               // x22 = vm count
	adr	x19, m_vms
	bl	d_puts
	mov	w23, w22
	bl	d_puthex2
	adr	x19, m_nl
	bl	d_puts

	// --- 2. per-VM state + runs ---
	mov	x21, #0               // id
2:	cmp	x21, x22
	b.ge	3f
	adr	x19, m_vm
	bl	d_puts
	mov	w23, w21
	bl	d_puthex2             // id
	adr	x19, m_st
	bl	d_puts
	mov	x1, #VMCTL_STATE
	mov	x2, x21
	bl	vmctl
	mov	w23, w0
	bl	d_puthex2             // packed state
	adr	x19, m_rn
	bl	d_puts
	mov	x1, #VMCTL_RUNS
	mov	x2, x21
	bl	vmctl
	mov	w23, w0
	bl	d_puthex2             // run_count (low byte)
	adr	x19, m_nl
	bl	d_puts
	add	x21, x21, #1
	b	2b
3:

	// --- 3. pause VM 1 (the heartbeat guest) ---
	adr	x19, m_stop
	bl	d_puts
	mov	x1, #VMCTL_STOP
	mov	x2, #1
	bl	vmctl

	// --- 4. idle ~2s so the heartbeat guest is visibly silent ---
	mov	x20, #40
4:	bl	d_yield
	subs	x20, x20, #1
	b.ne	4b

	// --- 5. resume VM 1 ---
	adr	x19, m_start
	bl	d_puts
	mov	x1, #VMCTL_START
	mov	x2, #1
	bl	vmctl

	// --- 6. warm-reset VM 2 (IPC producer) ---
	adr	x19, m_reset
	bl	d_puts
	mov	x1, #VMCTL_RESET
	mov	x2, #2
	bl	vmctl

	// --- 7. idle a bit so the other VMs accumulate exits, then dump stats ---
	mov	x20, #40
14:	bl	d_yield
	subs	x20, x20, #1
	b.ne	14b

	adr	x19, m_stats
	bl	d_puts
	mov	x21, #0                   // vm id
15:	cmp	x21, x22
	b.ge	16f
	adr	x19, m_svm
	bl	d_puts
	mov	w23, w21
	bl	d_puthex2                 // id
	adr	x19, m_colon
	bl	d_puts
	// 5 stat counters (hvc,dabt,sysreg,wfx,irq) — print low byte of each.
	mov	x24, #0                   // stat index
17:	cmp	x24, #5
	b.ge	18f
	mov	x1, #VMCTL_STAT
	mov	x2, x21
	mov	x3, x24
	bl	vmctl
	mov	w23, w0
	bl	d_puthex2
	mov	w0, #' '
	bl	d_putc
	add	x24, x24, #1
	b	17b
18:	adr	x19, m_nl
	bl	d_puts
	add	x21, x21, #1
	b	15b
16:
	// --- 8. weighted scheduling demo: give the IPC producer (vm 2) 8x the CPU
	// share of the consumer (vm 3); idle a while; then read each VM's CPU time
	// and show the producer accrued much more (proportional share). ---
	adr	x19, m_wset
	bl	d_puts
	mov	x1, #VMCTL_WEIGHT
	mov	x2, #2                    // vm 2 = ipc-prod
	mov	x3, #8                    // weight 8
	bl	vmctl
	mov	x1, #VMCTL_WEIGHT
	mov	x2, #3                    // vm 3 = ipc-cons
	mov	x3, #1                    // weight 1
	bl	vmctl

	// idle ~4s so the weighted slices accumulate a clear difference.
	mov	x20, #80
20:	bl	d_yield
	subs	x20, x20, #1
	b.ne	20b

	// dump per-VM cpu time (high byte = coarse magnitude).
	adr	x19, m_cpu
	bl	d_puts
	mov	x21, #0
21:	cmp	x21, x22
	b.ge	22f
	adr	x19, m_svm
	bl	d_puts
	mov	w23, w21
	bl	d_puthex2
	adr	x19, m_colon
	bl	d_puts
	mov	x1, #VMCTL_CPUTIME
	mov	x2, x21
	bl	vmctl
	// print bits[31:24] of the tick count (coarse, fits 2 hex digits).
	lsr	x23, x0, #24
	bl	d_puthex2
	adr	x19, m_nl
	bl	d_puts
	add	x21, x21, #1
	b	21b
22:
	// --- 9. snapshot / restore demo on vm 1 (the heartbeat guest, 64 MiB) ---
	// Let it run a bit, SNAPSHOT it (capturing its current beat counter), let it
	// climb higher, then RESTORE — its beat number visibly jumps back.
	mov	x20, #30
23:	bl	d_yield
	subs	x20, x20, #1
	b.ne	23b

	adr	x19, m_snap
	bl	d_puts
	mov	x1, #VMCTL_SNAPSHOT
	mov	x2, #1                    // vm 1 = guest2 heartbeat
	bl	vmctl
	mov	w23, w0
	bl	d_puthex2                 // 00 = VMCTL_OK
	adr	x19, m_nl
	bl	d_puts

	// let vm 1 climb for a while
	mov	x20, #60
24:	bl	d_yield
	subs	x20, x20, #1
	b.ne	24b

	adr	x19, m_rest
	bl	d_puts
	mov	x1, #VMCTL_RESTORE
	mov	x2, #1
	bl	vmctl
	mov	w23, w0
	bl	d_puthex2
	adr	x19, m_nl
	bl	d_puts

	adr	x19, m_done
	bl	d_puts

	// --- 10. idle forever ---
9:	bl	d_yield
	b	9b

// print one char in w0. clobbers x9,x10,x12.
d_putc:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
19:	ldr	w12, [x10]
	tbnz	w12, #5, 19b
	str	w0, [x9]
	ret

// ---- helpers ----
// vmctl: x1=op, x2=target -> x0 result. Clobbers x0.
vmctl:
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16
	movk	x0, #0x0002           // HVC_FERMI_VMCTL
	hvc	#0
	ret
// vmctl_count -> x0
vmctl_count:
	mov	x16, x30
	mov	x1, #VMCTL_COUNT
	mov	x2, #0
	bl	vmctl
	mov	x30, x16
	ret
d_yield:
	mov	x0, #0
	movk	x0, #0xFE11, lsl #16  // HVC_YIELD
	hvc	#0
	ret

d_puts:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
5:	ldrb	w11, [x19], #1
	cbz	w11, 6f
7:	ldr	w12, [x10]
	tbnz	w12, #5, 7b
	str	w11, [x9]
	b	5b
6:	ret

// print byte in w23 as two hex digits. Clobbers x9,x10,x12,x15.
d_puthex2:
	ldr	x9, =UART_DR
	ldr	x10, =UART_FR
	lsr	w15, w23, #4
	and	w15, w15, #0xF
	cmp	w15, #10
	b.lt	8f
	add	w15, w15, #('A' - 10)
	b	10f
8:	add	w15, w15, #'0'
10:	ldr	w12, [x10]
	tbnz	w12, #5, 10b
	str	w15, [x9]
	and	w15, w23, #0xF
	cmp	w15, #10
	b.lt	11f
	add	w15, w15, #('A' - 10)
	b	12f
11:	add	w15, w15, #'0'
12:	ldr	w12, [x10]
	tbnz	w12, #5, 12b
	str	w15, [x9]
	ret

.section .rodata.dom0
m_hdr:   .asciz "\r\n[dom0] privileged control domain online\r\n"
m_vms:   .asciz "[dom0] VMCTL_COUNT vms=0x"
m_vm:    .asciz "[dom0]  vm id=0x"
m_st:    .asciz " state=0x"
m_rn:    .asciz " runs=0x"
m_stop:  .asciz "[dom0] VMCTL_STOP vm 1 (pause heartbeat guest)\r\n"
m_start: .asciz "[dom0] VMCTL_START vm 1 (resume)\r\n"
m_reset: .asciz "[dom0] VMCTL_RESET vm 2 (warm-restart IPC producer)\r\n"
m_done:  .asciz "[dom0] management script complete; idling\r\n"
m_stats: .asciz "[dom0] exit stats per vm (hvc dabt sysreg wfx irq):\r\n"
m_wset:  .asciz "[dom0] VMCTL_WEIGHT: vm 2 = 8, vm 3 = 1 (proportional share)\r\n"
m_cpu:   .asciz "[dom0] cpu time per vm (cntpct bits[31:24]):\r\n"
m_snap:  .asciz "[dom0] VMCTL_SNAPSHOT vm 1 -> rc=0x"
m_rest:  .asciz "[dom0] VMCTL_RESTORE vm 1 (rollback) -> rc=0x"
m_svm:   .asciz "[dom0]  vm 0x"
m_colon: .asciz ": "
m_nl:    .asciz "\r\n"
