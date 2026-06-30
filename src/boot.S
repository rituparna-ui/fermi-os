.section .text.boot
.global _start

.equ KERNEL_VA_OFFSET, 0xFFFF000000000000
.equ EL2_STACK_SIZE,   8192
.equ EL3_STACK_SIZE,   8192
.equ RMM_STACK_SIZE,   8192
.equ SPSR_EL1H_DAIF,   0x3c5   // M=EL1h(0b0101), DAIF masked(0b1111<<6)
.equ SPSR_EL2H_DAIF,   0x3c9   // M=EL2h(0b1001), DAIF masked(0b1111<<6)

// SCR_EL3 bits used to drop into the Non-secure world running AArch64.
.equ SCR_NS,  (1 << 0)   // NS=1     -> Non-secure (with NSE=0)
.equ SCR_SMD, (1 << 7)   // (left 0: SMC enabled)
.equ SCR_HCE, (1 << 8)   // HVC instruction enabled at EL2/EL1
.equ SCR_RW,  (1 << 10)  // next lower level is AArch64

_start:
	// Symbols are linked at upper-half VAs. Before any MMU is on we need
	// PHYSICAL addresses, so subtract the offset. Set a provisional stack
	// for whichever EL we entered at.
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical stack top
	mov sp, x0

	// QEMU entry EL depends on the machine:
	//   secure=on,virtualization=on  -> EL3 (Root world, FEAT_RME)
	//   virtualization=on            -> EL2
	//   (neither)                    -> EL1
	mrs x0, CurrentEL
	lsr x0, x0, #2
	cmp x0, #3
	b.eq el3_path
	cmp x0, #2
	b.eq el2_path
	b el1_path

// --------------------------------------------------------------------------
// EL3: Root-world Secure Monitor bring-up. Initialise the monitor (UART, and
// — added in E0b — the Granule Protection Table + GPC), then ERET down to the
// Non-secure world to run Fermi (EL2 hypervisor + EL1 kernel) as the host.
// --------------------------------------------------------------------------
el3_path:
	// Dedicated EL3 stack (el3_init may use it). Physical address pre-MMU.
	ldr x0, =el3_stack
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	add x0, x0, #EL3_STACK_SIZE
	mov sp, x0

	bl el3_init			// C monitor init (runs at EL3, MMU off)

	// Launch the RMM in the Realm world at Realm-EL2. Set SP_EL2 to the RMM
	// stack, ELR_EL3 to the RMM entry (physical), SPSR to EL2h, and select
	// the Realm world via SCR_EL3.{NSE,NS} = 0b11. The RMM will SMC back to
	// EL3 (RMM_BOOT_COMPLETE), and the EL3 handler then enters Non-secure.
	ldr x0, =rmm_stack
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	add x0, x0, #RMM_STACK_SIZE
	msr sp_el2, x0

	ldr x0, =rmm_realm_main
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical entry
	msr elr_el3, x0
	mov x0, #SPSR_EL2H_DAIF
	msr spsr_el3, x0

	// SCR_EL3 = Realm world (NSE=1, NS=1) + HCE + RW.
	mov x0, #(SCR_NS | SCR_HCE | SCR_RW)
	mov x1, #1
	lsl x1, x1, #62			// NSE (bit 62)
	orr x0, x0, x1
	msr scr_el3, x0
	isb
	eret				// -> Realm-EL2 (rmm_realm_main)

// --------------------------------------------------------------------------
// EL2: Non-secure hypervisor bring-up (entered natively at EL2, or via the
// EL3 SMC handler's eret after the RMM boots). Configure stage-2 etc. and
// drop to EL1.
// --------------------------------------------------------------------------
.global el2_path
el2_path:
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	mov sp, x0			// re-establish a physical stack at EL2

	bl hyp_init

	// Repoint SP_EL2 at a dedicated EL2 stack so guest->EL2 traps don't
	// clobber the EL1 kernel stack.
	ldr x0, =el2_stack
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	add x0, x0, #EL2_STACK_SIZE
	mov sp, x0

	adr x0, el1_path
	msr elr_el2, x0
	mov x0, #SPSR_EL1H_DAIF
	msr spsr_el2, x0
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1
	msr sp_el1, x0
	eret				// -> el1_path at EL1

// --------------------------------------------------------------------------
// EL1: the Fermi kernel proper.
// --------------------------------------------------------------------------
el1_path:
	ldr x0, =__stack_top
	ldr x1, =KERNEL_VA_OFFSET
	sub x0, x0, x1			// physical stack top
	mov sp, x0

	// PC is physical. early_init is linked upper-half but bl is PC-relative,
	// so it branches to physical early_init. early_init enables the stage-1
	// MMU.
	bl early_init

	// MMU now on, TTBR1 maps upper half. Switch to upper-half VAs directly.
	ldr x0, =__stack_top
	mov sp, x0

	ldr x1, =kernel_main
	br x1

	// panic if kernel_main returns
	ldr x1, =kernel_panic_return
	br x1
