/*
 * boot.S — Early boot for aarch64 higher-half kernel
 *
 * Runs entirely at physical addresses (TTBR0 identity map).
 * Sets up minimal page tables for TTBR0 (identity) and TTBR1 (kernel
 * at 0xFFFF_0000_0000_0000), enables the MMU, then jumps to
 * kernel_main in the higher-half.
 *
 * Page table layout (4 KB granule, 48-bit VA, L1 1 GB blocks):
 *
 *   TTBR0  L0[0] → ttbr0_l1
 *          L1[0..9] = identity-mapped 1 GB blocks (covers 10 GB)
 *
 *   TTBR1  L0[0] → ttbr1_l1
 *          L1[0..9] = 1 GB blocks (same PA, different VA range)
 *
 *   Entry 0 = device memory (MMIO, AttrIdx=0)
 *   Entries 1-9 = normal memory (RAM, AttrIdx=1)
 *
 * QEMU virt with -m 8G:
 *   PA 0x0000_0000 – 0x3FFF_FFFF  device MMIO   (1 GB)
 *   PA 0x4000_0000 – 0x23FFF_FFFF RAM           (8 GB)
 */

.section .text.boot
.global _start

_start:
	/* ------------------------------------------------------------ */
	/* 1. Set up a temporary physical stack                         */
	/* ------------------------------------------------------------ */
	ldr	x0, =__kernel_end_phys
	add	x0, x0, #0x4000		/* 16 KB scratch stack past kernel image */
	mov	sp, x0

	/* ------------------------------------------------------------ */
	/* 2. Zero out the BSS section                                  */
	/* ------------------------------------------------------------ */
	ldr	x0, =__bss_start_phys
	ldr	x1, =__bss_end_phys
1:
	cmp	x0, x1
	b.ge	2f
	str	xzr, [x0], #8
	b	1b
2:

	/* ------------------------------------------------------------ */
	/* 3. Zero out boot page table pages (4 × 4 KB = 16 KB)        */
	/* ------------------------------------------------------------ */
	ldr	x0, =ttbr0_l0
	mov	x1, #(4096 * 4)		/* 4 pages */
	bl	_zero_mem

	/* ------------------------------------------------------------ */
	/* 4. Build TTBR0 page tables (identity map)                    */
	/* ------------------------------------------------------------ */
	ldr	x0, =ttbr0_l0
	ldr	x1, =ttbr0_l1

	/* L0[0] = table descriptor pointing to L1 */
	orr	x2, x1, #0x3			/* PTE_VALID | PTE_TABLE */
	str	x2, [x0, #0]

	/* Fill L1[0..9] with 1 GB block descriptors */
	mov	x3, #0				/* L1 index */
	mov	x4, #0				/* current PA (1 GB aligned) */
	mov	x7, #1
	lsl	x7, x7, #30			/* x7 = 1 GB constant */
_ttbr0_l1_loop:
	cmp	x3, #10
	b.ge	_ttbr0_l1_done

	/* Choose AttrIdx: 0 for entry 0 (device), 1 for the rest (normal) */
	cmp	x3, #0
	b.ne	_ttbr0_normal
	/* Device block: AttrIdx=0 */
	mov	x5, #0				/* AttrIdx(0) << 2 = 0 */
	b	_ttbr0_set_entry
_ttbr0_normal:
	mov	x5, #(1 << 2)			/* AttrIdx(1) << 2 = 4 */
_ttbr0_set_entry:
	mov	x6, x4				/* output address */
	orr	x6, x6, #(1 << 0)		/* PTE_VALID */
	/* bit[1]=0 already → block descriptor */
	orr	x6, x6, x5			/* AttrIdx */
	orr	x6, x6, #(1 << 10)		/* AF */
	orr	x6, x6, #(3 << 8)		/* SH = inner shareable */
	/* AP = 0 → EL1 RW (bits [7:6] = 00, already zero) */
	str	x6, [x1, x3, lsl #3]		/* L1[x3] = x6 */

	add	x3, x3, #1
	add	x4, x4, x7			/* += 1 GB */
	b	_ttbr0_l1_loop
_ttbr0_l1_done:

	/* ------------------------------------------------------------ */
	/* 5. Build TTBR1 page tables (kernel higher-half)              */
	/*    VA 0xFFFF_0000_0000_0000 + PA                             */
	/*    Exact same physical mapping — only the VA range differs.  */
	/* ------------------------------------------------------------ */
	ldr	x0, =ttbr1_l0
	ldr	x1, =ttbr1_l1

	/* L0[0] = table descriptor pointing to L1 */
	orr	x2, x1, #0x3
	str	x2, [x0, #0]

	/* Fill L1[0..9] — identical to TTBR0 */
	mov	x3, #0
	mov	x4, #0
	/* x7 still holds 1 GB from the TTBR0 loop */
_ttbr1_l1_loop:
	cmp	x3, #10
	b.ge	_ttbr1_l1_done

	cmp	x3, #0
	b.ne	_ttbr1_normal
	mov	x5, #0
	b	_ttbr1_set_entry
_ttbr1_normal:
	mov	x5, #(1 << 2)
_ttbr1_set_entry:
	mov	x6, x4
	orr	x6, x6, #(1 << 0)
	orr	x6, x6, x5
	orr	x6, x6, #(1 << 10)
	orr	x6, x6, #(3 << 8)
	str	x6, [x1, x3, lsl #3]

	add	x3, x3, #1
	add	x4, x4, x7			/* += 1 GB */
	b	_ttbr1_l1_loop
_ttbr1_l1_done:

	/* ------------------------------------------------------------ */
	/* 6. Configure MAIR_EL1                                        */
	/*    Attr0 = 0x00  Device-nGnRnE                               */
	/*    Attr1 = 0xFF  Normal, Write-Back, Read/Write Allocate     */
	/* ------------------------------------------------------------ */
	mov	x0, #0x00			/* AttrIdx 0 = Device */
	movk	x0, #0x00FF, lsl #0		/* AttrIdx 0 | AttrIdx 1 (0xFF << 8) */
	/* Build value more clearly */
	mov	x0, #0xFF00			/* Attr1 = 0xFF in bits [15:8] */
	/* Attr0 = 0x00 in bits [7:0] — already zero */
	msr	mair_el1, x0

	/* ------------------------------------------------------------ */
	/* 7. Configure TCR_EL1                                         */
	/*    T0SZ  = 16  → 48-bit VA for TTBR0 (bits [5:0])           */
	/*    IRGN0 = 01  → WB-WA  (bits [9:8])                        */
	/*    ORGN0 = 01  → WB-WA  (bits [11:10])                      */
	/*    SH0   = 11  → Inner Shareable (bits [13:12])              */
	/*    TG0   = 00  → 4 KB granule (bits [15:14])                 */
	/*    T1SZ  = 16  → 48-bit VA for TTBR1 (bits [21:16])         */
	/*    IRGN1 = 01  (bits [25:24])                                */
	/*    ORGN1 = 01  (bits [27:26])                                */
	/*    SH1   = 11  (bits [29:28])                                */
	/*    TG1   = 10  → 4 KB granule (bits [31:30])                 */
	/*    IPS   = 010 → 40-bit PA (bits [34:32])                    */
	/* ------------------------------------------------------------ */
	mov	x0, #0x10			/* T0SZ = 16 */
	orr	x0, x0, #(0x10 << 16)		/* T1SZ = 16 */
	orr	x0, x0, #(0x01 << 8)		/* IRGN0 */
	orr	x0, x0, #(0x01 << 10)		/* ORGN0 */
	orr	x0, x0, #(0x03 << 12)		/* SH0 */
	orr	x0, x0, #(0x01 << 24)		/* IRGN1 */
	orr	x0, x0, #(0x01 << 26)		/* ORGN1 */
	orr	x0, x0, #(0x03 << 28)		/* SH1 */
	orr	x0, x0, #(0x02 << 30)		/* TG1 = 4 KB */
	/* IPS = 010 (40-bit) → bit 33 */
	mov	x1, #(0x02)
	lsl	x1, x1, #32
	orr	x0, x0, x1
	msr	tcr_el1, x0

	/* ------------------------------------------------------------ */
	/* 8. Load page table base registers                            */
	/* ------------------------------------------------------------ */
	ldr	x0, =ttbr0_l0
	ldr	x1, =ttbr1_l0

	dsb	ishst
	msr	ttbr0_el1, x0
	msr	ttbr1_el1, x1
	dsb	ish
	isb

	/* Invalidate all TLBs */
	tlbi	vmalle1
	dsb	ish
	isb

	/* ------------------------------------------------------------ */
	/* 9. Enable the MMU                                            */
	/* ------------------------------------------------------------ */
	mrs	x0, sctlr_el1
	orr	x0, x0, #(1 << 0)		/* M  — MMU enable */
	orr	x0, x0, #(1 << 2)		/* C  — data cache */
	orr	x0, x0, #(1 << 12)		/* I  — instruction cache */
	msr	sctlr_el1, x0
	isb

	/* ------------------------------------------------------------ */
	/* 10. Jump to kernel_main in the higher half                   */
	/*     Switch the stack pointer to its virtual address too.     */
	/* ------------------------------------------------------------ */
	ldr	x0, =__stack_top		/* virtual address from linker */
	mov	sp, x0

	ldr	x0, =kernel_main		/* virtual address */
	br	x0

	/* Should never return */
_halt:
	wfe
	b	_halt

/* ------------------------------------------------------------------ */
/* Helper: zero x1 bytes at address x0.  Clobbers x0, x1, x2.        */
/* ------------------------------------------------------------------ */
_zero_mem:
	cbz	x1, _zero_done
	strb	wzr, [x0], #1
	sub	x1, x1, #1
	b	_zero_mem
_zero_done:
	ret

/* ================================================================== */
/* Static page table storage (placed in .bss, zeroed above)           */
/* Each table is 4096 bytes (512 × 8-byte entries), 4 KB aligned.     */
/* ================================================================== */
.section .bss.boot
.balign 4096
ttbr0_l0:	.skip 4096
ttbr0_l1:	.skip 4096
ttbr1_l0:	.skip 4096
ttbr1_l1:	.skip 4096
