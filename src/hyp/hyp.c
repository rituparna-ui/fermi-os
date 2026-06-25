#include "hyp.h"
#include "hyp/hypercall.h" /* HVC_* ABI */
#include "gic/gic.h"    /* GICR_* / GICD_* for the hypervisor timer PPI */
#include "mmio/mmio.h"  /* mmio_read32 / mmio_write32 */
#include "mm/mmu/mmu.h" /* _1GB, _512GB */
#include "mm/pmm/pmm.h" /* MEM_START, MEM_SIZE */
#include "strings/strings.h" /* memset, memcpy */
#include "uart/uart.h"  /* UART_BASE / UART_DR / UART_FR */

/* ---------------------------------------------------------------------------
 * hyp.c — EL2 hypervisor core (Milestone 1)
 *
 * Responsibilities at this milestone:
 *   1. Log that we entered at EL2 (via a self-contained PL011 writer — the
 *      normal uart driver isn't initialised until early_init, which runs
 *      later at EL1).
 *   2. Let the EL1 guest reach the generic timer (CNTHCTL_EL2 / CNTVOFF_EL2).
 *   3. Build a flat stage-2 (IPA == PA) identity map so the guest's existing
 *      physical view is preserved, and enable it (HCR_EL2.VM).
 *   4. Install the EL2 vector table (VBAR_EL2) so guest traps land in EL2.
 *
 * The eret down to EL1 is performed by boot.S after this returns.
 * --------------------------------------------------------------------------- */

#define MSR(reg, val)                                                          \
  do {                                                                         \
    uint64_t _v = (val);                                                       \
    __asm__ __volatile__("msr " #reg ", %0" ::"r"(_v));                        \
  } while (0)

#define MRS(reg)                                                               \
  ({                                                                           \
    uint64_t _v;                                                               \
    __asm__ __volatile__("mrs %0, " #reg : "=r"(_v));                          \
    _v;                                                                        \
  })

/* PL011 TXFF (transmit FIFO full) lives in the flag register, bit 5. */
#define UART_FR_TXFF (1U << 5)

/* Dedicated EL2 stack for trap handling. SP_EL2 is repointed here by boot.S
 * before the eret, so guest->EL2 traps never clobber the EL1 kernel stack
 * (which SP_EL1 keeps using). Exported so boot.S can compute its top. */
__attribute__((aligned(16), section(".hyp_tables"))) uint8_t el2_stack[8192];

/* Stage-2 page tables. 4 KiB granule, 48-bit IPA input (T0SZ=16, start at
 * level 0), 40-bit PA output. We use 1 GiB blocks at level 1, so the whole
 * 0..1 TiB IPA space is described by exactly three tables — no L2/L3 needed.
 *
 * These live in .bss (NOLOAD) but are filled explicitly below, so we do not
 * depend on zero_bss() (which only runs later, at EL1). */
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l0[512];
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l1_low[512];  /* 0..512 GiB  */
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l1_high[512]; /* 512G..1 TiB */

/* Extra tables used to split the single 1 GiB block that contains the
 * hypervisor's own memory down to 4 KiB pages, so we can punch a hole and
 * deny the guest any stage-2 mapping of hypervisor-private RAM. */
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l2_split[512]; /* one 1 GiB region as 2 MiB blocks */
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l3_split[512]; /* one 2 MiB block as 4 KiB pages   */

/* Hypervisor-private region bounds (linker symbols, see linker.ld). Taken
 * pre-MMU/at EL2 their addresses are physical == guest IPA (identity map). */
extern uint8_t __hyp_start[];
extern uint8_t __hyp_end[];

/* Linux-slot guest (vCPU 1). Its RAM lives in a Fermi-invisible high physical
 * region (just past Fermi's 8 GiB PMM view); stage-2 maps the guest's IPA
 * window there, plus the PL011 UART so the guest can drive earlycon. */
#define LINUX_PHYS_BASE 0x240000000ULL /* 9 GiB: past Fermi's 8 GiB view      */
#define LINUX_IPA_BASE 0x40000000ULL   /* arm64 RAM base the guest sees       */
#define LINUX_RAM_SIZE (1024ULL * 1024 * 1024)
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t lx_l0[512];
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t lx_l1[512];
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t lx_l2_ram[512]; /* IPA 1-2 GiB (RAM)     */
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t lx_l2_dev[512]; /* IPA 0-1 GiB (devices) */

/* Third guest (vCPU 2): a tiny silent bare-metal payload in its own isolated
 * 2 MiB slice of Fermi-invisible high RAM (just past the Linux window). It
 * uses only hypercalls — no UART, no GIC — so its stage-2 maps only its RAM. */
#define GUEST2_PHYS_BASE 0x280000000ULL /* 10 GiB                              */
#define GUEST2_IPA_BASE 0x40000000ULL   /* its own private IPA view            */
#define GUEST2_RAM_SIZE _2MB

/* The vCPUs and the index of the one currently running. In .hyp_tables
 * (NOLOAD); initialised explicitly in hyp_init(). */
__attribute__((section(".hyp_tables"))) static vcpu_t vcpus[NUM_VCPUS];
__attribute__((section(".hyp_tables"))) static int current_vcpu;
__attribute__((section(".hyp_tables"))) static uint64_t g_switch_count;
__attribute__((section(".hyp_tables"))) static uint64_t g_wfi_count;
__attribute__((section(".hyp_tables"))) static uint64_t g_midr;

/* Captured Linux-guest console. The Linux PL011 is left unmapped in its
 * stage-2, so its UART MMIO traps to EL2 and is emulated (hyp_emulate_pl011);
 * output bytes are appended here and read back by Fermi via /proc. Linear
 * capture (stops when full) — holds the boot log plus the first shell. */
#define LCON_SZ (32 * 1024)
__attribute__((section(".hyp_tables"))) static uint8_t g_lcon[LCON_SZ];
__attribute__((section(".hyp_tables"))) static uint32_t g_lcon_len;

/* virtio-blk RAM disk (declared early so hyp_init can seed it; the device
 * model is defined further below). */
/* virtio-blk is backed by an 8 MiB ext4 image staged by QEMU's loader into
 * Fermi-invisible high RAM at phys 0x280000000 (just past the Linux window).
 * EL2 runs MMU-off, so the hypervisor reaches it physically; neither guest can
 * see it (outside Fermi's view and the Linux stage-2 window). */
#define VBLK_PHYS_BASE 0x280000000ULL
#define VBLK_BYTES (8ULL * 1024 * 1024) /* 8 MiB */
#define VBLK_SECTORS (VBLK_BYTES / 512)
#define g_vdisk ((uint8_t *)VBLK_PHYS_BASE)

/* Emulated PL011 RX side (for interactive input to the Linux guest). Bytes
 * pushed via HVC_LCON_PUT land in this FIFO; when the guest has enabled the RX
 * interrupt (IMSC.RXIM) the hypervisor injects the UART SPI so Linux's pl011
 * IRQ handler drains the FIFO (via DR reads) into its tty. */
#define LRX_SZ 256
__attribute__((section(".hyp_tables"))) static uint8_t g_lrx[LRX_SZ];
__attribute__((section(".hyp_tables"))) static uint32_t g_lrx_head; /* write */
__attribute__((section(".hyp_tables"))) static uint32_t g_lrx_tail; /* read  */
__attribute__((section(".hyp_tables"))) static uint32_t g_pl011_imsc; /* mask */

#define PL011_RXIM (1u << 4) /* IMSC/RIS/MIS receive-interrupt bit */
#define PL011_RTIM (1u << 6) /* IMSC/RIS/MIS receive-timeout-interrupt bit */
#define UART_SPI_INTID 33    /* DT: pl011 interrupts = <0 1 4> => SPI 1 => 33 */

static inline int lrx_empty(void) { return g_lrx_head == g_lrx_tail; }

/* Push one byte / a string into the guest UART RX FIFO (dropping on overflow). */
static void lrx_push(uint8_t b) {
  uint32_t nh = (g_lrx_head + 1) % LRX_SZ;
  if (nh != g_lrx_tail) {
    g_lrx[g_lrx_head] = b;
    g_lrx_head = nh;
  }
}
static void lrx_push_str(const char *s) {
  while (*s)
    lrx_push((uint8_t)*s++);
}
static void hyp_uart_rx_kick(void);
/* CNTHP (EL2 physical timer) scheduling tick. */
#define HYP_TIMER_INTID 26   /* PPI 26 = non-secure EL2 physical timer */
#define HYP_QUANTUM_MS 10    /* preemption time-slice (short: keeps cross-vCPU
                              * IPI / stop_machine latency low for SMP guests) */
__attribute__((section(".hyp_tables"))) static uint64_t g_quantum_ticks;

static void hyp_create_linux_guest(void);
static void hyp_create_linux_secondary(void);
static void hyp_tick_init(void);
static void hyp_vgic_inject_ex(int target, uint32_t intid, int hw);
static void hyp_tick_start(void);

/* --- self-contained PL011 output (no driver state, safe pre-uart_init) --- */
static void hyp_putc(char c) {
  volatile uint32_t *fr = (volatile uint32_t *)UART_FR;
  volatile uint32_t *dr = (volatile uint32_t *)UART_DR;
  while (*fr & UART_FR_TXFF) {
  }
  *dr = (uint32_t)c;
}

static void hyp_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      hyp_putc('\r');
    hyp_putc(*s++);
  }
}

static void hyp_puthex(uint64_t v) {
  hyp_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uint64_t nib = (v >> i) & 0xF;
    hyp_putc((char)(nib < 10 ? '0' + nib : 'a' + (nib - 10)));
  }
}

/* Build the IPA==PA identity map with 1 GiB blocks.
 *   - RAM [MEM_START, MEM_START+MEM_SIZE)            -> Normal WB
 *   - everything else (GIC, UART, PCI ECAM/MMIO)     -> Device-nGnRnE
 * Pointers are physical here (PC-relative, MMU off), which is exactly what
 * the descriptors and VTTBR_EL2 must contain. */
static void hyp_build_stage2(void) {
  const uint64_t mem_end = MEM_START + MEM_SIZE;

  /* L0: only the first two 512 GiB regions are populated. */
  for (int i = 0; i < 512; i++)
    s2_l0[i] = 0;
  s2_l0[0] = ((uint64_t)s2_l1_low) | S2_TABLE | S2_VALID;
  s2_l0[1] = ((uint64_t)s2_l1_high) | S2_TABLE | S2_VALID;

  /* L1 low: 512 x 1 GiB blocks covering IPA 0 .. 512 GiB. */
  for (uint64_t i = 0; i < 512; i++) {
    uint64_t pa = i * _1GB;
    uint64_t mem = (pa >= MEM_START && pa < mem_end) ? S2_MEM_NORMAL
                                                     : S2_MEM_DEVICE;
    uint64_t sh = (mem == S2_MEM_NORMAL) ? S2_SH_INNER : 0;
    s2_l1_low[i] = pa | S2_VALID | S2_AF | sh | S2_AP_RW | mem;
  }

  /* L1 high: 512 x 1 GiB blocks covering IPA 512 GiB .. 1 TiB — all device
   * (the PCI MMIO64 window). */
  for (uint64_t i = 0; i < 512; i++) {
    uint64_t pa = _512GB + i * _1GB;
    s2_l1_high[i] = pa | S2_VALID | S2_AF | S2_AP_RW | S2_MEM_DEVICE;
  }

  /* --- Isolation: deny the guest any stage-2 mapping of hypervisor RAM ---
   *
   * The hypervisor's private region [__hyp_start, __hyp_end) lives inside one
   * 1 GiB block of RAM. Split that block: 1 GiB -> 512x2 MiB (s2_l2_split),
   * and the single 2 MiB block that contains the region -> 512x4 KiB
   * (s2_l3_split). Then mark the 4 KiB pages covering the hyp region invalid.
   * The hardware table walker reaches these split tables via VTTBR physical
   * addresses, so unmapping them from the guest IPA view is safe. */
  const uint64_t PG = 4096ULL;
  uint64_t hs = (uint64_t)__hyp_start;
  uint64_t he = (uint64_t)__hyp_end;
  uint64_t gb_idx = hs / _1GB;                 /* which s2_l1_low entry      */
  uint64_t region_base = gb_idx * _1GB;
  uint64_t mb_idx = (hs - region_base) / _2MB; /* 2 MiB block within region  */
  uint64_t mb_base = region_base + mb_idx * _2MB;

  /* L2 split: identity 2 MiB blocks for the whole 1 GiB RAM region. */
  for (uint64_t b = 0; b < 512; b++) {
    uint64_t pa = region_base + b * _2MB;
    s2_l2_split[b] =
        pa | S2_VALID | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
  }

  /* L3 split: identity 4 KiB pages for the 2 MiB block holding the hyp
   * region, with the hyp pages left invalid (unmapped). Page descriptors at
   * L3 use bits[1:0]=11 (S2_TABLE encoding). */
  for (uint64_t p = 0; p < 512; p++) {
    uint64_t pa = mb_base + p * PG;
    if (pa >= (hs & ~(PG - 1)) && pa < he) {
      s2_l3_split[p] = 0; /* hole: hypervisor-private, guest has no access */
    } else {
      s2_l3_split[p] =
          pa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
    }
  }

  /* Splice the split tables in, replacing the original 1 GiB block. */
  s2_l2_split[mb_idx] = ((uint64_t)s2_l3_split) | S2_TABLE | S2_VALID;
  s2_l1_low[gb_idx] = ((uint64_t)s2_l2_split) | S2_TABLE | S2_VALID;

  __asm__ __volatile__("dsb ish");
}

void hyp_init(void) {
  hyp_puts("\n[HYP] Fermi hypervisor online at EL2\n");

  /* vCPU table starts empty; populated near the end of hyp_init once the
   * stage-2 tables and guest 1 payload are in place. */
  memset(vcpus, 0, sizeof(vcpus));
  current_vcpu = 0;

  /* Sanity: confirm we really are at EL2. */
  uint64_t el = (MRS(CurrentEL) >> 2) & 0x3;
  hyp_puts("[HYP] CurrentEL = ");
  hyp_puthex(el);
  hyp_puts("\n");

  /* Generic timer: zero the virtual offset and let EL1/EL0 use the physical
   * counter and timer registers directly (Fermi drives the timer from EL1). */
  MSR(cntvoff_el2, 0);
  {
    uint64_t cnthctl = MRS(cnthctl_el2);
    cnthctl |= (CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN);
    MSR(cnthctl_el2, cnthctl);
  }

  /* Stage-2 translation tables (with the hypervisor's own RAM unmapped). */
  hyp_build_stage2();
  hyp_puts("[HYP] isolated hyp region [");
  hyp_puthex((uint64_t)__hyp_start);
  hyp_puts(", ");
  hyp_puthex((uint64_t)__hyp_end);
  hyp_puts(") from guest stage-2\n");

  /* VTCR_EL2: 4 KiB granule, 48-bit IPA (T0SZ=16, SL0=2 => start at L0),
   * 40-bit PA output (PS=2 => 1 TiB), inner-shareable WB walks. */
  uint64_t vtcr = (16ULL << 0) |  /* T0SZ = 16 -> 48-bit IPA          */
                  (2ULL << 6) |   /* SL0  = 2  -> start at level 0    */
                  (1ULL << 8) |   /* IRGN0 = WB/WA                    */
                  (1ULL << 10) |  /* ORGN0 = WB/WA                    */
                  (3ULL << 12) |  /* SH0   = inner shareable          */
                  (0ULL << 14) |  /* TG0   = 4 KiB granule            */
                  (2ULL << 16);   /* PS    = 40-bit (1 TiB) PA        */
  MSR(vtcr_el2, vtcr);

  /* VTTBR_EL2: physical base of the stage-2 L0 table, VMID = 0. */
  MSR(vttbr_el2, (uint64_t)s2_l0);

  /* Install the EL2 vector table (defined in vector_el2.S). Address is
   * physical here (MMU off), which is what VBAR_EL2 needs. */
  extern uint8_t el2_vector_table[];
  MSR(vbar_el2, (uint64_t)el2_vector_table);

  __asm__ __volatile__("isb");

  /* Allow EL2 itself to use FP/SIMD — we execute stp/ldp q-regs to context-
   * switch guest FP state. (CPTR_EL2.TFP=0 => FP not trapped to EL2.) */
  {
    uint64_t cptr = MRS(cptr_el2);
    cptr &= ~(1ULL << 10); /* TFP */
    MSR(cptr_el2, cptr);
    __asm__ __volatile__("isb");
  }

  /* --- GICv3 virtualization bring-up ---
   * Own the physical CPU interface at EL2 so physical IRQs (routed here by
   * HCR_EL2.IMO below) can be acked, and enable the virtual CPU interface so
   * we can inject virtual interrupts the guest consumes on ICV_*.
   *
   * EOImode=1 makes our physical EOIR1 a priority-drop only; the actual
   * deactivation is deferred to the guest via hardware-linked list regs. */
  MSR(icc_sre_el2, ICC_SRE_SRE | ICC_SRE_ENABLE);
  __asm__ __volatile__("isb");
  MSR(icc_pmr_el1, 0xFFULL);                 /* accept all priorities (phys) */
  {
    uint64_t ctlr = MRS(icc_ctlr_el1);
    ctlr |= ICC_CTLR_EOIMODE;
    MSR(icc_ctlr_el1, ctlr);
  }
  MSR(icc_igrpen1_el1, 1ULL);                /* enable phys Group 1 at EL2   */
  MSR(ich_hcr_el2, ICH_HCR_EN | ICH_HCR_TC); /* enable vCPU iface; trap SGIs */
  __asm__ __volatile__("isb");

  /* Enable stage-2, pin EL1 to AArch64, route physical IRQs to EL2 (IMO) so we
   * can inject vIRQs, and trap guest WFI (TWI) so an idle guest yields its slice
   * to the others instead of halting the physical CPU until the next tick.
   * (TID3 ID-register trapping from the M2 demo is left off here — guests,
   * including Linux, read ID registers natively.) */
  MSR(hcr_el2, HCR_RW | HCR_VM | HCR_IMO | HCR_TWI);
  __asm__ __volatile__("isb");

  /* Virtualized CPU identity: the guest's MPIDR_EL1 / MIDR_EL1 reads come from
   * VMPIDR_EL2 / VPIDR_EL2. Seed them so the primary guest sees a sane core id;
   * each vCPU's MPIDR is then restored per world-switch (per-core for SMP). */
  g_midr = MRS(midr_el1);
  uint64_t real_mpidr = MRS(mpidr_el1);
  MSR(vpidr_el2, g_midr);
  MSR(vmpidr_el2, real_mpidr);
  __asm__ __volatile__("isb");

  /* Register the primary guest (Fermi) as vCPU 0 — its full context is
   * captured lazily on its first yield — then create the Linux guest and its
   * second core (an SMP secondary, parked until PSCI CPU_ON). */
  vcpus[0].id = 0;
  vcpus[0].state = VCPU_RUNNING;
  vcpus[0].mpidr = real_mpidr;
  vcpus[0].vttbr = (uint64_t)s2_l0; /* VMID 0 */
  current_vcpu = 0;
  hyp_create_linux_guest();
  hyp_puts("[HYP] created Linux-slot guest (vCPU 1): 1 GiB @ IPA 0x40000000\n");
  hyp_create_linux_secondary();
  hyp_puts("[HYP] prepared Linux SMP secondary (vCPU 2), parked for CPU_ON\n");

  /* Start the preemptive scheduling tick (CNTHP / PPI 26). */
  hyp_tick_init();
  hyp_tick_start();
  hyp_puts("[HYP] preemptive scheduler armed (CNTHP tick)\n");

  hyp_puts("[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

/* ------------------------ world switch / vCPUs ----------------------------- */

static void hyp_save_el1(vcpu_t *v) {
  v->sp_el1 = MRS(sp_el1);
  v->sp_el0 = MRS(sp_el0);
  v->elr_el1 = MRS(elr_el1);
  v->spsr_el1 = MRS(spsr_el1);
  v->sctlr_el1 = MRS(sctlr_el1);
  v->cpacr_el1 = MRS(cpacr_el1);
  v->ttbr0_el1 = MRS(ttbr0_el1);
  v->ttbr1_el1 = MRS(ttbr1_el1);
  v->tcr_el1 = MRS(tcr_el1);
  v->mair_el1 = MRS(mair_el1);
  v->amair_el1 = MRS(amair_el1);
  v->vbar_el1 = MRS(vbar_el1);
  v->contextidr_el1 = MRS(contextidr_el1);
  v->tpidr_el1 = MRS(tpidr_el1);
  v->tpidrro_el0 = MRS(tpidrro_el0);
  v->tpidr_el0 = MRS(tpidr_el0);
  v->esr_el1 = MRS(esr_el1);
  v->far_el1 = MRS(far_el1);
  v->par_el1 = MRS(par_el1);
  v->cntv_ctl = MRS(cntv_ctl_el0);
  v->cntv_cval = MRS(cntv_cval_el0);
}

static void hyp_restore_el1(vcpu_t *v) {
  MSR(sp_el1, v->sp_el1);
  MSR(sp_el0, v->sp_el0);
  MSR(elr_el1, v->elr_el1);
  MSR(spsr_el1, v->spsr_el1);
  MSR(sctlr_el1, v->sctlr_el1);
  MSR(cpacr_el1, v->cpacr_el1);
  MSR(ttbr0_el1, v->ttbr0_el1);
  MSR(ttbr1_el1, v->ttbr1_el1);
  MSR(tcr_el1, v->tcr_el1);
  MSR(mair_el1, v->mair_el1);
  MSR(amair_el1, v->amair_el1);
  MSR(vbar_el1, v->vbar_el1);
  MSR(contextidr_el1, v->contextidr_el1);
  MSR(tpidr_el1, v->tpidr_el1);
  MSR(tpidrro_el0, v->tpidrro_el0);
  MSR(tpidr_el0, v->tpidr_el0);
  MSR(esr_el1, v->esr_el1);
  MSR(far_el1, v->far_el1);
  MSR(par_el1, v->par_el1);
  MSR(cntv_ctl_el0, v->cntv_ctl);
  MSR(cntv_cval_el0, v->cntv_cval);
  MSR(vmpidr_el2, v->mpidr); /* the core identity the guest reads as MPIDR_EL1 */
  MSR(vpidr_el2, g_midr);
  __asm__ __volatile__("isb");
}

static void hyp_save_fp(vcpu_t *v) {
  uint64_t *p = v->vregs;
  __asm__ __volatile__(
      "stp q0, q1, [%0, #0]\n\t stp q2, q3, [%0, #32]\n\t"
      "stp q4, q5, [%0, #64]\n\t stp q6, q7, [%0, #96]\n\t"
      "stp q8, q9, [%0, #128]\n\t stp q10, q11, [%0, #160]\n\t"
      "stp q12, q13, [%0, #192]\n\t stp q14, q15, [%0, #224]\n\t"
      "stp q16, q17, [%0, #256]\n\t stp q18, q19, [%0, #288]\n\t"
      "stp q20, q21, [%0, #320]\n\t stp q22, q23, [%0, #352]\n\t"
      "stp q24, q25, [%0, #384]\n\t stp q26, q27, [%0, #416]\n\t"
      "stp q28, q29, [%0, #448]\n\t stp q30, q31, [%0, #480]\n\t" ::"r"(p)
      : "memory");
  v->fpsr = MRS(fpsr);
  v->fpcr = MRS(fpcr);
}

static void hyp_restore_fp(vcpu_t *v) {
  uint64_t *p = v->vregs;
  MSR(fpsr, v->fpsr);
  MSR(fpcr, v->fpcr);
  __asm__ __volatile__(
      "ldp q0, q1, [%0, #0]\n\t ldp q2, q3, [%0, #32]\n\t"
      "ldp q4, q5, [%0, #64]\n\t ldp q6, q7, [%0, #96]\n\t"
      "ldp q8, q9, [%0, #128]\n\t ldp q10, q11, [%0, #160]\n\t"
      "ldp q12, q13, [%0, #192]\n\t ldp q14, q15, [%0, #224]\n\t"
      "ldp q16, q17, [%0, #256]\n\t ldp q18, q19, [%0, #288]\n\t"
      "ldp q20, q21, [%0, #320]\n\t ldp q22, q23, [%0, #352]\n\t"
      "ldp q24, q25, [%0, #384]\n\t ldp q26, q27, [%0, #416]\n\t"
      "ldp q28, q29, [%0, #448]\n\t ldp q30, q31, [%0, #480]\n\t" ::"r"(p)
      : "memory");
}

/* Per-guest vGIC state save/restore. */
static void hyp_save_vgic(vcpu_t *v) {
  __asm__ __volatile__("mrs %0, ich_lr0_el2" : "=r"(v->ich_lr[0]));
  __asm__ __volatile__("mrs %0, ich_lr1_el2" : "=r"(v->ich_lr[1]));
  v->ich_vmcr = MRS(ich_vmcr_el2);
  v->ich_ap1r0 = MRS(ich_ap1r0_el2);
}

static void hyp_restore_vgic(vcpu_t *v) {
  __asm__ __volatile__("msr ich_lr0_el2, %0" ::"r"(v->ich_lr[0]));
  __asm__ __volatile__("msr ich_lr1_el2, %0" ::"r"(v->ich_lr[1]));
  MSR(ich_vmcr_el2, v->ich_vmcr);
  MSR(ich_ap1r0_el2, v->ich_ap1r0);
  __asm__ __volatile__("isb");
}

/* Round-robin to the next non-unused vCPU after `from`. */
static int hyp_pick_next(int from) {
  for (int i = 1; i <= NUM_VCPUS; i++) {
    int idx = (from + i) % NUM_VCPUS;
    if (vcpus[idx].state != VCPU_UNUSED)
      return idx;
  }
  return from;
}

/* Cooperative world switch: suspend the running vCPU and resume the next.
 * The outgoing GP regs come from / incoming GP regs go to the EL2 trap frame
 * (which el2_common restores on eret). Each vCPU has a distinct VMID, so no
 * stage-2 TLB flush is needed when swapping VTTBR_EL2. */
static void hyp_world_switch(el2_frame_t *f) {
  vcpu_t *cur = &vcpus[current_vcpu];

  hyp_save_fp(cur); /* first: guest FP is still live, C path hasn't used SIMD */
  for (int i = 0; i < 31; i++)
    cur->regs[i] = f->x[i];
  cur->pc = MRS(elr_el2);
  cur->pstate = MRS(spsr_el2);
  cur->vttbr = MRS(vttbr_el2);
  hyp_save_el1(cur);
  hyp_save_vgic(cur);
  if (cur->state == VCPU_RUNNING)
    cur->state = VCPU_READY;

  int next = hyp_pick_next(current_vcpu);
  if (next == current_vcpu) {
    cur->state = VCPU_RUNNING; /* nobody else runnable: keep going */
    return;
  }

  vcpu_t *nv = &vcpus[next];
  current_vcpu = next;
  nv->state = VCPU_RUNNING;
  g_switch_count++;

  for (int i = 0; i < 31; i++)
    f->x[i] = nv->regs[i];
  MSR(elr_el2, nv->pc);
  MSR(spsr_el2, nv->pstate);
  MSR(vttbr_el2, nv->vttbr);
  __asm__ __volatile__("isb");
  hyp_restore_el1(nv);
  hyp_restore_vgic(nv);
  hyp_restore_fp(nv); /* last: nothing in the C path touches SIMD after this */
}

/* Build the Linux-slot guest's stage-2: a 256 MiB RAM window at IPA
 * 0x40000000 backed by Fermi-invisible physical RAM, plus the PL011 UART. */
static void hyp_build_linux_stage2(void) {
  for (int i = 0; i < 512; i++) {
    lx_l0[i] = 0;
    lx_l1[i] = 0;
    lx_l2_ram[i] = 0;
    lx_l2_dev[i] = 0;
  }
  lx_l0[0] = ((uint64_t)lx_l1) | S2_TABLE | S2_VALID;
  lx_l1[0] = ((uint64_t)lx_l2_dev) | S2_TABLE | S2_VALID; /* IPA 0..1 GiB    */
  lx_l1[1] = ((uint64_t)lx_l2_ram) | S2_TABLE | S2_VALID; /* IPA 1..2 GiB    */

  /* RAM: IPA [0x40000000, +256 MiB) -> phys [LINUX_PHYS_BASE, +256 MiB). */
  uint64_t blocks = LINUX_RAM_SIZE / _2MB;
  uint64_t base_idx = (LINUX_IPA_BASE % _1GB) / _2MB; /* = 0 */
  for (uint64_t i = 0; i < blocks; i++) {
    uint64_t pa = LINUX_PHYS_BASE + i * _2MB;
    lx_l2_ram[base_idx + i] =
        pa | S2_VALID | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
  }

  /* NOTE: the PL011 UART (IPA 0x09000000) is deliberately NOT mapped here.
   * Leaving it unmapped makes the guest's UART MMIO trap to EL2, where it is
   * emulated and its output captured into g_lcon (see hyp_emulate_pl011),
   * exposed to Fermi as /proc/linux_console — so the Linux console no longer
   * interleaves with Fermi's on the shared serial. */

  __asm__ __volatile__("dsb ish");
}

static void hyp_create_linux_guest(void) {
  hyp_build_linux_stage2();

  /* Isolation: remove the Linux guest's RAM window (and its staged Image/DTB/
   * initramfs) from the PRIMARY guest's stage-2, so Fermi cannot see or
   * corrupt Linux's memory. The window is a whole number of 1 GiB stage-2
   * blocks in Fermi's map; clearing them is enough (done before the first
   * eret to EL1, so no stage-2 TLB flush is required). */
  for (uint64_t i = 0; i < LINUX_RAM_SIZE / _1GB; i++)
    s2_l1_low[LINUX_PHYS_BASE / _1GB + i] = 0;
  __asm__ __volatile__("dsb ish");

  /* The Linux Image and DTB are staged into the guest's high RAM by QEMU's
   * generic loader (see Makefile), at IPAs 0x40200000 and 0x48000000. We just
   * enter per the arm64 boot protocol: PC = Image base, x0 = DTB, EL1h, MMU
   * off, x1..x3 = 0. */
  vcpu_t *v = &vcpus[1];
  memset(v, 0, sizeof(*v));
  v->id = 1;
  v->state = VCPU_READY;
  v->mpidr = 0x80000000ULL;                     /* core 0 (aff0=0, bit31 RES1) */
  v->pc = LINUX_IPA_BASE + 0x200000;            /* Image entry (IPA)         */
  v->regs[0] = LINUX_IPA_BASE + 0x8000000;      /* x0 = DTB (IPA 0x48000000) */
  v->pstate = 0x3c5;                            /* EL1h, DAIF masked         */
  v->vttbr = ((uint64_t)lx_l0) | (1ULL << 48);  /* VMID 1                    */
  /* sctlr_el1 = 0 => stage-1 MMU off, as Linux's early entry expects. */
}

/* Linux SMP secondary (vCPU 2, core 1). It shares the primary Linux guest's
 * stage-2 (same VTTBR / VMID 1), so it sees the same RAM and devices. It starts
 * UNUSED (parked); Linux's boot CPU brings it online via PSCI CPU_ON, which
 * fills in its entry PC and marks it READY. */
static void hyp_create_linux_secondary(void) {
  vcpu_t *v = &vcpus[2];
  memset(v, 0, sizeof(*v));
  v->id = 2;
  v->state = VCPU_UNUSED;                        /* parked until CPU_ON       */
  v->mpidr = 0x80000001ULL;                      /* core 1 (aff0=1)           */
  v->vttbr = ((uint64_t)lx_l0) | (1ULL << 48);   /* shares Linux VMID 1       */
}

/* Bring up just enough of the physical GIC for the hypervisor's own timer
 * interrupt (PPI 26, CNTHP). The primary guest re-runs its full gic_init
 * later; these writes are idempotent / non-conflicting (set-only ISENABLER,
 * group bit kept set). EL2 runs MMU-off, so GIC MMIO is reached physically. */
static void hyp_tick_init(void) {
  /* Affinity routing + Group1 NS at the distributor (idempotent w/ guest). */
  mmio_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);

  /* Wake the redistributor. */
  uint32_t waker = mmio_read32(GICR_WAKER);
  waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
  mmio_write32(GICR_WAKER, waker);
  while (mmio_read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
  }

  /* PPI 26 (CNTHP, scheduler tick) and PPI 27 (CNTV, the Linux guest's
   * virtual timer) -> Group1 NS, enabled. */
  uint32_t grp = mmio_read32(GICR_IGROUPR0);
  grp |= (1u << HYP_TIMER_INTID) | (1u << 27);
  mmio_write32(GICR_IGROUPR0, grp);
  mmio_write32(GICR_ISENABLER0, (1u << HYP_TIMER_INTID) | (1u << 27));
}

/* Arm CNTHP_EL2 to fire one quantum from now. */
static void hyp_tick_start(void) {
  uint64_t freq = MRS(cntfrq_el0);
  g_quantum_ticks = freq * HYP_QUANTUM_MS / 1000;
  uint64_t now = MRS(cntpct_el0);
  MSR(cnthp_cval_el2, now + g_quantum_ticks);
  MSR(cnthp_ctl_el2, 1ULL); /* enable, unmasked */
}

/* ------------------------- emulated virtio-rng ----------------------------- */
/* A minimal virtio-mmio (version 2) virtio-rng device for the Linux guest. Its
 * MMIO window is unmapped in the guest's stage-2 so accesses trap to EL2. The
 * device has one virtqueue; on QueueNotify the hypervisor walks the guest's
 * split ring, fills each available buffer with pseudo-random bytes, publishes
 * the used ring, and injects the device's SPI. */
#define VIRTIO_LO 0x0a000000ULL
#define VIRTIO_HI 0x0a000200ULL
#define VIRTIO_RNG_INTID 34 /* DT: virtio interrupts = <0 2 4> => SPI 2 => 34 */

__attribute__((section(".hyp_tables"))) static struct {
  uint32_t status;
  uint32_t dev_feat_sel, drv_feat_sel;
  uint32_t q_num;
  uint32_t q_ready;
  uint64_t q_desc, q_avail, q_used;
  uint16_t last_avail;
  uint32_t int_status;
  uint64_t prng;
  uint32_t served;
} g_vrng;

/* Translate a Linux-guest IPA to a hypervisor-usable physical pointer (the
 * Linux RAM window is mapped linearly IPA 0x40000000 -> phys LINUX_PHYS_BASE).
 * Returns NULL for IPAs outside that window. */
static void *lx_gpa(uint64_t ipa) {
  if (ipa < LINUX_IPA_BASE || ipa >= LINUX_IPA_BASE + LINUX_RAM_SIZE)
    return 0;
  return (void *)(ipa - LINUX_IPA_BASE + LINUX_PHYS_BASE);
}

static uint8_t vrng_byte(void) {
  uint64_t x = g_vrng.prng ? g_vrng.prng : 0x9e3779b97f4a7c15ULL;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_vrng.prng = x;
  return (uint8_t)(x >> 24);
}

/* Split-virtqueue descriptor (matches the Linux/virtio layout). */
struct vq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

static void hyp_virtio_process(void) {
  if (!g_vrng.q_ready || !g_vrng.q_desc || !g_vrng.q_avail || !g_vrng.q_used)
    return;
  struct vq_desc *desc = lx_gpa(g_vrng.q_desc);
  volatile uint16_t *avail = lx_gpa(g_vrng.q_avail); /* [0]=flags [1]=idx ring[2..] */
  volatile uint16_t *used16 = lx_gpa(g_vrng.q_used); /* [0]=flags [1]=idx */
  if (!desc || !avail || !used16 || g_vrng.q_num == 0)
    return;

  uint16_t avail_idx = avail[1];
  int worked = 0;
  while (g_vrng.last_avail != avail_idx) {
    uint16_t slot = g_vrng.last_avail % g_vrng.q_num;
    uint16_t didx = avail[2 + slot];
    if (didx >= g_vrng.q_num)
      break;
    struct vq_desc *d = &desc[didx];
    uint8_t *buf = lx_gpa(d->addr);
    uint32_t len = d->len;
    if (buf)
      for (uint32_t i = 0; i < len; i++)
        buf[i] = vrng_byte();

    /* used ring entry at offset 4: struct { u32 id; u32 len; } ring[] */
    volatile uint32_t *used_ring = (volatile uint32_t *)(used16 + 2);
    uint16_t uidx = used16[1];
    uint16_t uslot = uidx % g_vrng.q_num;
    used_ring[uslot * 2 + 0] = didx;
    used_ring[uslot * 2 + 1] = len;
    __asm__ __volatile__("dsb ish");
    used16[1] = uidx + 1; /* publish */

    g_vrng.last_avail++;
    worked = 1;
  }
  if (worked) {
    __asm__ __volatile__("dsb ish");
    g_vrng.int_status |= 1; /* used-buffer notification */
    hyp_vgic_inject_ex(1 /* Linux */, VIRTIO_RNG_INTID, 0 /* SW */);
    if (g_vrng.served < 3) {
      g_vrng.served++;
      hyp_puts("[HYP] virtio-rng: served guest queue (");
      hyp_puthex(g_vrng.served);
      hyp_puts(")\n");
    }
  }
}

static int hyp_emulate_virtio(uint64_t ipa, int is_write, uint64_t *val) {
  uint64_t off = ipa - VIRTIO_LO;
  if (is_write) {
    switch (off) {
    case 0x014: g_vrng.dev_feat_sel = (uint32_t)*val; break;
    case 0x024: g_vrng.drv_feat_sel = (uint32_t)*val; break;
    case 0x030: /* QueueSel — only queue 0 exists */ break;
    case 0x038: g_vrng.q_num = (uint32_t)*val; break;
    case 0x044: g_vrng.q_ready = (uint32_t)*val; break;
    case 0x050: hyp_virtio_process(); break; /* QueueNotify */
    case 0x064: g_vrng.int_status &= ~(uint32_t)*val; break; /* InterruptACK */
    case 0x070: g_vrng.status = (uint32_t)*val; break;
    case 0x080: g_vrng.q_desc = (g_vrng.q_desc & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x084: g_vrng.q_desc = (g_vrng.q_desc & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x090: g_vrng.q_avail = (g_vrng.q_avail & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x094: g_vrng.q_avail = (g_vrng.q_avail & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x0a0: g_vrng.q_used = (g_vrng.q_used & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x0a4: g_vrng.q_used = (g_vrng.q_used & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    default: break;
    }
    return 1;
  }
  switch (off) {
  case 0x000: *val = 0x74726976; break;       /* Magic "virt"               */
  case 0x004: *val = 2; break;                /* Version 2 (modern)         */
  case 0x008: *val = 4; break;                /* DeviceID 4 = entropy/rng   */
  case 0x00c: *val = 0x554d4551; break;       /* VendorID "QEMU"            */
  case 0x010: /* DeviceFeatures: only VIRTIO_F_VERSION_1 (bit 32) */
    *val = (g_vrng.dev_feat_sel == 1) ? (1u << 0) : 0;
    break;
  case 0x034: *val = 8; break;                /* QueueNumMax                */
  case 0x044: *val = g_vrng.q_ready; break;   /* QueueReady                 */
  case 0x060: *val = g_vrng.int_status; break;/* InterruptStatus            */
  case 0x070: *val = g_vrng.status; break;    /* Status                     */
  case 0x0fc: *val = 0; break;                /* ConfigGeneration           */
  default: *val = 0; break;
  }
  return 1;
}

/* ------------------------- emulated virtio-blk ----------------------------- */
/* A minimal virtio-mmio (version 2) virtio-blk device backed by an in-
 * hypervisor RAM disk. Exposes /dev/vda to the Linux guest. On QueueNotify the
 * hypervisor walks each request's descriptor chain (16-byte out-header, one or
 * more data buffers, a 1-byte status) and reads/writes the RAM disk. */
#define VBLK_LO 0x0a000200ULL
#define VBLK_HI 0x0a000400ULL
#define VBLK_INTID 35 /* DT: virtio_blk interrupts = <0 3 4> => SPI 3 => 35 */
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VQ_F_NEXT 1

__attribute__((section(".hyp_tables"))) static struct {
  uint32_t status;
  uint32_t dev_feat_sel, drv_feat_sel;
  uint32_t q_num, q_ready;
  uint64_t q_desc, q_avail, q_used;
  uint16_t last_avail;
  uint32_t int_status;
  uint32_t served;
} g_vblk;

static void hyp_vblk_process(void) {
  if (!g_vblk.q_ready || !g_vblk.q_desc || !g_vblk.q_avail || !g_vblk.q_used)
    return;
  struct vq_desc *desc = lx_gpa(g_vblk.q_desc);
  volatile uint16_t *avail = lx_gpa(g_vblk.q_avail);
  volatile uint16_t *used16 = lx_gpa(g_vblk.q_used);
  if (!desc || !avail || !used16 || g_vblk.q_num == 0)
    return;

  uint16_t avail_idx = avail[1];
  int worked = 0;
  while (g_vblk.last_avail != avail_idx) {
    uint16_t head = avail[2 + (g_vblk.last_avail % g_vblk.q_num)];
    if (head >= g_vblk.q_num)
      break;

    struct vq_desc *d = &desc[head];
    uint8_t *hdr = lx_gpa(d->addr);
    uint32_t type = 0;
    uint64_t sector = 0;
    if (hdr) {
      type = *(volatile uint32_t *)(hdr + 0);
      sector = *(volatile uint64_t *)(hdr + 8);
    }
    uint64_t off = sector * 512;
    uint32_t total = 0;

    if (d->flags & VQ_F_NEXT) {
      uint16_t idx = d->next;
      for (;;) {
        if (idx >= g_vblk.q_num)
          break;
        struct vq_desc *dd = &desc[idx];
        if (!(dd->flags & VQ_F_NEXT)) { /* status byte (device writes) */
          uint8_t *st = lx_gpa(dd->addr);
          if (st)
            st[0] = 0; /* VIRTIO_BLK_S_OK */
          total += 1;
          break;
        }
        uint8_t *buf = lx_gpa(dd->addr);
        uint32_t len = dd->len;
        if (buf && off + len <= VBLK_BYTES) {
          if (type == VIRTIO_BLK_T_IN)
            memcpy(buf, g_vdisk + off, len);
          else if (type == VIRTIO_BLK_T_OUT)
            memcpy(g_vdisk + off, buf, len);
        }
        off += len;
        total += len;
        idx = dd->next;
      }
    }

    volatile uint32_t *used_ring = (volatile uint32_t *)(used16 + 2);
    uint16_t uidx = used16[1];
    uint16_t uslot = uidx % g_vblk.q_num;
    used_ring[uslot * 2 + 0] = head;
    used_ring[uslot * 2 + 1] = total;
    __asm__ __volatile__("dsb ish");
    used16[1] = uidx + 1;

    g_vblk.last_avail++;
    worked = 1;
  }
  if (worked) {
    __asm__ __volatile__("dsb ish");
    g_vblk.int_status |= 1;
    hyp_vgic_inject_ex(1 /* Linux */, VBLK_INTID, 0 /* SW */);
    if (g_vblk.served < 3) {
      g_vblk.served++;
      hyp_puts("[HYP] virtio-blk: served guest request\n");
    }
  }
}

static int hyp_emulate_vblk(uint64_t ipa, int is_write, uint64_t *val) {
  uint64_t off = ipa - VBLK_LO;
  if (is_write) {
    switch (off) {
    case 0x014: g_vblk.dev_feat_sel = (uint32_t)*val; break;
    case 0x024: g_vblk.drv_feat_sel = (uint32_t)*val; break;
    case 0x030: break; /* QueueSel — single queue */
    case 0x038: g_vblk.q_num = (uint32_t)*val; break;
    case 0x044: g_vblk.q_ready = (uint32_t)*val; break;
    case 0x050: hyp_vblk_process(); break; /* QueueNotify */
    case 0x064: g_vblk.int_status &= ~(uint32_t)*val; break;
    case 0x070: g_vblk.status = (uint32_t)*val; break;
    case 0x080: g_vblk.q_desc = (g_vblk.q_desc & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x084: g_vblk.q_desc = (g_vblk.q_desc & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x090: g_vblk.q_avail = (g_vblk.q_avail & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x094: g_vblk.q_avail = (g_vblk.q_avail & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x0a0: g_vblk.q_used = (g_vblk.q_used & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x0a4: g_vblk.q_used = (g_vblk.q_used & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    default: break;
    }
    return 1;
  }
  switch (off) {
  case 0x000: *val = 0x74726976; break;       /* Magic                       */
  case 0x004: *val = 2; break;                /* Version 2                   */
  case 0x008: *val = 2; break;                /* DeviceID 2 = block          */
  case 0x00c: *val = 0x554d4551; break;       /* VendorID                    */
  case 0x010: *val = (g_vblk.dev_feat_sel == 1) ? (1u << 0) : 0; break; /* VERSION_1 */
  case 0x034: *val = 8; break;                /* QueueNumMax                 */
  case 0x044: *val = g_vblk.q_ready; break;
  case 0x060: *val = g_vblk.int_status; break;
  case 0x070: *val = g_vblk.status; break;
  case 0x0fc: *val = 0; break;                /* ConfigGeneration            */
  case 0x100: *val = VBLK_SECTORS; break;     /* config: capacity[31:0]      */
  case 0x104: *val = 0; break;                /* config: capacity[63:32]     */
  default: *val = 0; break;
  }
  return 1;
}

/* ------------------------- emulated virtio-net ----------------------------- */
/* A virtio-mmio (version 2) network device (DeviceID 1) => eth0. It has two
 * virtqueues (0 = RX guest<-host, 1 = TX guest->host). The hypervisor is the
 * link peer: on TX it parses the guest's ethernet frame and, for an ARP request
 * or ICMP echo addressed to OUR_IP, builds a reply and delivers it on the RX
 * queue. So the guest can `ping` a hypervisor-emulated host end-to-end. */
#define VNET_LO 0x0a000400ULL
#define VNET_HI 0x0a000600ULL
#define VNET_INTID 36 /* DT: virtio_net interrupts = <0 4 4> => SPI 4 => 36 */
#define VRING_F_NEXT 1
#define VNET_HDR_LEN 12 /* sizeof(struct virtio_net_hdr_v1) with VERSION_1 */

static const uint8_t OUR_MAC[6] = {0x52, 0x54, 0x00, 0xfe, 0x7b, 0x01};
static const uint8_t OUR_IP[4] = {10, 0, 0, 1};

struct vnet_q {
  uint32_t num, ready;
  uint64_t desc, avail, used;
  uint16_t last_avail;
};
__attribute__((section(".hyp_tables"))) static struct {
  uint32_t status;
  uint32_t dev_feat_sel, drv_feat_sel;
  uint32_t int_status;
  uint32_t q_sel;
  struct vnet_q q[2]; /* 0 = RX, 1 = TX */
  uint32_t tx_seen, rx_sent;
} g_vnet;
__attribute__((section(".hyp_tables"))) static uint8_t g_net_tx[2048];
__attribute__((section(".hyp_tables"))) static uint8_t g_net_rx[2048];

/* RFC1071 ones-complement checksum over network-order bytes. */
static uint16_t in_csum(const uint8_t *p, uint32_t len) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i + 1 < len; i += 2)
    sum += ((uint32_t)p[i] << 8) | p[i + 1];
  if (len & 1)
    sum += (uint32_t)p[len - 1] << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

/* Gather a descriptor chain into buf; returns bytes copied. */
static uint32_t vnet_gather(struct vq_desc *desc, uint16_t head, uint32_t qnum,
                            uint8_t *buf, uint32_t maxlen) {
  uint32_t total = 0;
  uint16_t idx = head;
  for (int guard = 0; guard < 128; guard++) {
    struct vq_desc *d = &desc[idx];
    uint8_t *p = lx_gpa(d->addr);
    if (p)
      for (uint32_t i = 0; i < d->len && total < maxlen; i++)
        buf[total++] = p[i];
    if (!(d->flags & VRING_F_NEXT))
      break;
    idx = d->next;
    if (idx >= qnum)
      break;
  }
  return total;
}

/* Scatter buf into a (device-writable) descriptor chain; returns bytes written. */
static uint32_t vnet_scatter(struct vq_desc *desc, uint16_t head, uint32_t qnum,
                             const uint8_t *buf, uint32_t len) {
  uint32_t total = 0;
  uint16_t idx = head;
  for (int guard = 0; guard < 128; guard++) {
    struct vq_desc *d = &desc[idx];
    uint8_t *p = lx_gpa(d->addr);
    if (p)
      for (uint32_t i = 0; i < d->len && total < len; i++)
        p[i] = buf[total++];
    if (total >= len || !(d->flags & VRING_F_NEXT))
      break;
    idx = d->next;
    if (idx >= qnum)
      break;
  }
  return total;
}

/* Deliver a single ethernet frame (already in g_net_rx + VNET_HDR_LEN) of
 * length framelen to the guest's RX queue. Returns 1 if delivered. */
static int hyp_vnet_rx_submit(uint32_t framelen) {
  struct vnet_q *q = &g_vnet.q[0];
  if (!q->ready || !q->desc || !q->avail || !q->used || q->num == 0)
    return 0;
  struct vq_desc *desc = lx_gpa(q->desc);
  volatile uint16_t *avail = lx_gpa(q->avail);
  volatile uint16_t *used16 = lx_gpa(q->used);
  if (!desc || !avail || !used16)
    return 0;
  if (q->last_avail == avail[1])
    return 0; /* no RX buffer posted -> drop */
  uint16_t head = avail[2 + (q->last_avail % q->num)];
  if (head >= q->num)
    return 0;

  /* virtio_net_hdr_v1: zeroed, num_buffers = 1 (offset 10). */
  for (int i = 0; i < VNET_HDR_LEN; i++)
    g_net_rx[i] = 0;
  g_net_rx[10] = 1;
  uint32_t total = VNET_HDR_LEN + framelen;
  uint32_t w = vnet_scatter(desc, head, q->num, g_net_rx, total);

  volatile uint32_t *ur = (volatile uint32_t *)(used16 + 2);
  uint16_t uidx = used16[1];
  uint16_t us = uidx % q->num;
  ur[us * 2 + 0] = head;
  ur[us * 2 + 1] = w;
  __asm__ __volatile__("dsb ish");
  used16[1] = uidx + 1;
  q->last_avail++;
  g_vnet.rx_sent++;
  return 1;
}

/* Parse one guest TX ethernet frame and, if it's an ARP request or ICMP echo
 * for OUR_IP, build a reply into g_net_rx+VNET_HDR_LEN and submit it on RX. */
static void hyp_vnet_handle_frame(const uint8_t *f, uint32_t len) {
  if (len < 14)
    return;
  uint8_t *r = &g_net_rx[VNET_HDR_LEN];
  uint16_t eth = ((uint16_t)f[12] << 8) | f[13];

  if (eth == 0x0806 && len >= 42) { /* ARP */
    const uint8_t *a = f + 14;
    uint16_t oper = ((uint16_t)a[6] << 8) | a[7];
    const uint8_t *tpa = a + 24;
    if (oper != 1 || tpa[0] != OUR_IP[0] || tpa[1] != OUR_IP[1] ||
        tpa[2] != OUR_IP[2] || tpa[3] != OUR_IP[3])
      return;
    const uint8_t *sha = a + 8, *spa = a + 14;
    /* ethernet */
    for (int i = 0; i < 6; i++) r[i] = sha[i];        /* dst = requester */
    for (int i = 0; i < 6; i++) r[6 + i] = OUR_MAC[i];/* src = us        */
    r[12] = 0x08; r[13] = 0x06;
    /* arp reply */
    uint8_t *ra = r + 14;
    ra[0] = 0; ra[1] = 1;            /* htype ethernet */
    ra[2] = 0x08; ra[3] = 0x00;      /* ptype IPv4     */
    ra[4] = 6; ra[5] = 4;            /* hlen, plen     */
    ra[6] = 0; ra[7] = 2;            /* oper = reply   */
    for (int i = 0; i < 6; i++) ra[8 + i] = OUR_MAC[i];
    for (int i = 0; i < 4; i++) ra[14 + i] = OUR_IP[i];
    for (int i = 0; i < 6; i++) ra[18 + i] = sha[i];
    for (int i = 0; i < 4; i++) ra[24 + i] = spa[i];
    hyp_vnet_rx_submit(42);
    return;
  }

  if (eth == 0x0800 && len >= 14 + 20) { /* IPv4 */
    const uint8_t *ip = f + 14;
    uint32_t ihl = (ip[0] & 0x0f) * 4;
    uint16_t iplen = ((uint16_t)ip[2] << 8) | ip[3];
    uint8_t proto = ip[9];
    const uint8_t *sip = ip + 12, *dip = ip + 16;
    if (proto != 1 || ihl < 20 || (uint32_t)14 + iplen > len)
      return;
    if (dip[0] != OUR_IP[0] || dip[1] != OUR_IP[1] || dip[2] != OUR_IP[2] ||
        dip[3] != OUR_IP[3])
      return;
    const uint8_t *icmp = ip + ihl;
    if (icmp[0] != 8) /* echo request */
      return;
    uint32_t framelen = 14 + iplen;
    if (framelen > sizeof(g_net_rx) - VNET_HDR_LEN)
      return;
    for (uint32_t i = 0; i < framelen; i++) /* start from the request */
      r[i] = f[i];
    /* swap ethernet src/dst */
    for (int i = 0; i < 6; i++) { r[i] = f[6 + i]; r[6 + i] = OUR_MAC[i]; }
    /* swap IP src/dst, reset TTL, recompute header checksum */
    uint8_t *rip = r + 14;
    for (int i = 0; i < 4; i++) { rip[12 + i] = dip[i]; rip[16 + i] = sip[i]; }
    rip[8] = 64;            /* TTL */
    rip[10] = rip[11] = 0;  /* checksum field */
    uint16_t ic = in_csum(rip, ihl);
    rip[10] = ic >> 8; rip[11] = ic & 0xff;
    /* ICMP: echo reply, recompute checksum */
    uint8_t *ricmp = rip + ihl;
    uint32_t icmplen = iplen - ihl;
    ricmp[0] = 0;                 /* type = echo reply */
    ricmp[2] = ricmp[3] = 0;      /* checksum field    */
    uint16_t cc = in_csum(ricmp, icmplen);
    ricmp[2] = cc >> 8; ricmp[3] = cc & 0xff;
    hyp_vnet_rx_submit(framelen);
    return;
  }
}

static void hyp_vnet_tx(void) {
  struct vnet_q *q = &g_vnet.q[1];
  if (!q->ready || !q->desc || !q->avail || !q->used || q->num == 0)
    return;
  struct vq_desc *desc = lx_gpa(q->desc);
  volatile uint16_t *avail = lx_gpa(q->avail);
  volatile uint16_t *used16 = lx_gpa(q->used);
  if (!desc || !avail || !used16)
    return;
  uint16_t aidx = avail[1];
  int worked = 0;
  while (q->last_avail != aidx) {
    uint16_t head = avail[2 + (q->last_avail % q->num)];
    if (head >= q->num)
      break;
    uint32_t n = vnet_gather(desc, head, q->num, g_net_tx, sizeof(g_net_tx));

    volatile uint32_t *ur = (volatile uint32_t *)(used16 + 2);
    uint16_t uidx = used16[1];
    uint16_t us = uidx % q->num;
    ur[us * 2 + 0] = head;
    ur[us * 2 + 1] = n;
    __asm__ __volatile__("dsb ish");
    used16[1] = uidx + 1;
    q->last_avail++;
    worked = 1;
    g_vnet.tx_seen++;
    if (n > VNET_HDR_LEN)
      hyp_vnet_handle_frame(g_net_tx + VNET_HDR_LEN, n - VNET_HDR_LEN);
  }
  if (worked) {
    __asm__ __volatile__("dsb ish");
    g_vnet.int_status |= 1;
    hyp_vgic_inject_ex(1 /* Linux */, VNET_INTID, 0 /* SW */);
  }
}

static int hyp_emulate_vnet(uint64_t ipa, int is_write, uint64_t *val) {
  uint64_t off = ipa - VNET_LO;
  uint32_t s = g_vnet.q_sel < 2 ? g_vnet.q_sel : 0;
  if (is_write) {
    switch (off) {
    case 0x014: g_vnet.dev_feat_sel = (uint32_t)*val; break;
    case 0x024: g_vnet.drv_feat_sel = (uint32_t)*val; break;
    case 0x030: g_vnet.q_sel = (uint32_t)*val; break;
    case 0x038: g_vnet.q[s].num = (uint32_t)*val; break;
    case 0x044: g_vnet.q[s].ready = (uint32_t)*val; break;
    case 0x050: if ((uint32_t)*val == 1) hyp_vnet_tx(); break; /* QueueNotify */
    case 0x064: g_vnet.int_status &= ~(uint32_t)*val; break;
    case 0x070: g_vnet.status = (uint32_t)*val; break;
    case 0x080: g_vnet.q[s].desc = (g_vnet.q[s].desc & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x084: g_vnet.q[s].desc = (g_vnet.q[s].desc & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x090: g_vnet.q[s].avail = (g_vnet.q[s].avail & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x094: g_vnet.q[s].avail = (g_vnet.q[s].avail & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    case 0x0a0: g_vnet.q[s].used = (g_vnet.q[s].used & ~0xFFFFFFFFULL) | (uint32_t)*val; break;
    case 0x0a4: g_vnet.q[s].used = (g_vnet.q[s].used & 0xFFFFFFFFULL) | ((uint64_t)*val << 32); break;
    default: break;
    }
    return 1;
  }
  if (off >= 0x100 && off < 0x106) { /* config: MAC address */
    *val = OUR_MAC[off - 0x100];
    return 1;
  }
  switch (off) {
  case 0x000: *val = 0x74726976; break;       /* Magic                       */
  case 0x004: *val = 2; break;                /* Version 2                   */
  case 0x008: *val = 1; break;                /* DeviceID 1 = network        */
  case 0x00c: *val = 0x554d4551; break;       /* VendorID                    */
  case 0x010: /* DeviceFeatures: VIRTIO_NET_F_MAC (bit5) + VERSION_1 (bit32) */
    *val = (g_vnet.dev_feat_sel == 1) ? (1u << 0) : (1u << 5);
    break;
  case 0x034: *val = 256; break;              /* QueueNumMax                 */
  case 0x044: *val = g_vnet.q[s].ready; break;
  case 0x060: *val = g_vnet.int_status; break;
  case 0x070: *val = g_vnet.status; break;
  case 0x0fc: *val = 0; break;                /* ConfigGeneration            */
  default: *val = 0; break;
  }
  return 1;
}

/* ------------------------------- traps ------------------------------------ */

static const char *ec_name(uint64_t ec) {
  switch (ec) {
  case EC_HVC64:
    return "HVC (hypercall)";
  case EC_SYSREG:
    return "trapped MSR/MRS";
  case EC_DABT_LOWER:
    return "data abort (stage-2)";
  case EC_IABT_LOWER:
    return "instruction abort (stage-2)";
  default:
    return "other";
  }
}

/* HVC hypercall: function ID in x0, args in x1..x3, result back in x0.
 * ELR_EL2 already points past the HVC, so no PC adjustment is needed. */
static void hyp_world_switch(el2_frame_t *f);

/* PSCI function IDs (SMC Calling Convention, 32-bit space). */
#define PSCI_VERSION_FN 0x84000000
#define PSCI_CPU_OFF 0x84000002
#define PSCI_MIGRATE_INFO_TYPE 0x84000006
#define PSCI_SYSTEM_OFF 0x84000008
#define PSCI_SYSTEM_RESET 0x84000009
#define PSCI_FEATURES 0x8400000A
#define PSCI_CPU_ON_64 0xC4000003
#define PSCI_AFFINITY_INFO_64 0xC4000004

/* Reap the running vCPU and switch to the next runnable one without saving
 * the dying vCPU's state. If none remain, halt. */
static void hyp_vcpu_exit(el2_frame_t *f) {
  vcpu_t *cur = &vcpus[current_vcpu];
  hyp_puts("[HYP] vCPU ");
  hyp_puthex(cur->id);
  hyp_puts(" powered off (PSCI)\n");
  cur->state = VCPU_UNUSED;

  int next = hyp_pick_next(current_vcpu);
  if (next == current_vcpu) {
    hyp_puts("[HYP] no runnable vCPUs left; halting\n");
    for (;;)
      __asm__ __volatile__("wfi");
  }

  vcpu_t *nv = &vcpus[next];
  current_vcpu = next;
  nv->state = VCPU_RUNNING;
  g_switch_count++;

  for (int i = 0; i < 31; i++)
    f->x[i] = nv->regs[i];
  MSR(elr_el2, nv->pc);
  MSR(spsr_el2, nv->pstate);
  MSR(vttbr_el2, nv->vttbr);
  __asm__ __volatile__("isb");
  hyp_restore_el1(nv);
  hyp_restore_vgic(nv);
  hyp_restore_fp(nv);
}

/* Minimal PSCI: report a version, boot/inspect SMP secondaries, and treat
 * SYSTEM_OFF / SYSTEM_RESET from a guest as "this VM is done" — reap it.
 * Returns nonzero if handled. */
static int hyp_handle_psci(el2_frame_t *f, uint64_t fn) {
  switch (fn) {
  case PSCI_VERSION_FN:
    f->x[0] = 0x00010001; /* PSCI v1.1 */
    return 1;
  case PSCI_FEATURES: {
    uint64_t qfn = f->x[1];
    /* Advertise the calls we implement (0 = present); else NOT_SUPPORTED. */
    if (qfn == PSCI_CPU_ON_64 || qfn == PSCI_AFFINITY_INFO_64 ||
        qfn == PSCI_CPU_OFF || qfn == PSCI_SYSTEM_OFF ||
        qfn == PSCI_SYSTEM_RESET || qfn == PSCI_VERSION_FN)
      f->x[0] = 0;
    else
      f->x[0] = (uint64_t)-1;
    return 1;
  }
  case PSCI_MIGRATE_INFO_TYPE:
    f->x[0] = 2; /* Trusted OS not present / migration not required */
    return 1;
  case PSCI_CPU_ON_64: {
    uint64_t target = f->x[1] & 0xFFFFFFULL; /* affinity bits of MPIDR */
    uint64_t entry = f->x[2];
    uint64_t ctx = f->x[3];
    for (int i = 0; i < NUM_VCPUS; i++) {
      if ((vcpus[i].mpidr & 0xFFFFFFULL) != target)
        continue;
      if (vcpus[i].state != VCPU_UNUSED) {
        f->x[0] = (uint64_t)-4; /* ALREADY_ON */
        return 1;
      }
      vcpu_t *v = &vcpus[i];
      uint64_t saved_mpidr = v->mpidr, saved_vttbr = v->vttbr, saved_id = v->id;
      memset(v, 0, sizeof(*v));
      v->id = saved_id;
      v->mpidr = saved_mpidr;
      v->vttbr = saved_vttbr; /* shares the Linux VM's stage-2 */
      v->pc = entry;          /* PSCI entry point (IPA), MMU off */
      v->regs[0] = ctx;       /* context_id passed in x0 */
      v->pstate = 0x3c5;      /* EL1h, DAIF masked */
      __asm__ __volatile__("dsb ish");
      v->state = VCPU_READY;  /* scheduler will run it on the next tick */
      hyp_puts("[HYP] PSCI CPU_ON: started vCPU ");
      hyp_puthex((uint64_t)i);
      hyp_puts(" @ entry ");
      hyp_puthex(entry);
      hyp_puts("\n");
      f->x[0] = 0; /* SUCCESS */
      return 1;
    }
    f->x[0] = (uint64_t)-2; /* INVALID_PARAMETERS */
    return 1;
  }
  case PSCI_AFFINITY_INFO_64: {
    uint64_t target = f->x[1] & 0xFFFFFFULL;
    f->x[0] = 1; /* default OFF */
    for (int i = 0; i < NUM_VCPUS; i++)
      if ((vcpus[i].mpidr & 0xFFFFFFULL) == target)
        f->x[0] = (vcpus[i].state == VCPU_UNUSED) ? 1 : 0; /* 0 = ON */
    return 1;
  }
  case PSCI_CPU_OFF:
  case PSCI_SYSTEM_OFF:
  case PSCI_SYSTEM_RESET:
    hyp_vcpu_exit(f); /* does not return to the caller's vCPU */
    return 1;
  default:
    return 0;
  }
}

static void hyp_handle_hvc(el2_frame_t *f) {
  uint64_t fn = f->x[0];
  uint64_t a1 = f->x[1];
  uint64_t a2 = f->x[2];
  uint64_t ret;
  vcpu_t *cur = &vcpus[current_vcpu];

  cur->hvc_count++;

  /* PSCI calls use the 0x8400_00xx / 0xC400_00xx function-ID space, distinct
   * from our small-integer hypercall ABI. Dispatch them first. */
  if ((fn & 0xFFFFFF00U) == 0x84000000U ||
      (fn & 0xFFFFFF00U) == 0xC4000000U) {
    if (hyp_handle_psci(f, fn))
      return;
    f->x[0] = (uint64_t)-1; /* PSCI NOT_SUPPORTED */
    return;
  }

  switch (fn) {
  case HVC_VERSION:
    ret = HYP_ABI_VERSION;
    break;
  case HVC_PUTC:
    hyp_putc((char)a1);
    ret = 0;
    break;
  case HVC_PING:
    ret = a1 + 1;
    break;
  case HVC_VM_INFO:
    ret = cur->hvc_count;
    break;
  case HVC_YIELD:
    /* Cooperative world switch: save this vCPU, resume the next ready one.
     * The frame's GP regs are rewritten in place, so we must NOT touch
     * f->x[0] afterwards — return immediately. */
    hyp_world_switch(f);
    return;
  case HVC_HYP_BASE:
    /* Introspection probe (test build): expose the hyp region base so the
     * guest can attempt — and be denied — an access to hypervisor memory. */
    ret = (uint64_t)__hyp_start;
    break;
  case HVC_VM_COUNT:
    ret = NUM_VCPUS;
    break;
  case HVC_LCON_LEN:
    ret = g_lcon_len;
    break;
  case HVC_LCON_PUT: {
    /* Enqueue one input byte for the Linux guest's emulated UART RX FIFO. */
    lrx_push((uint8_t)a1);
    ret = 0;
    break;
  }
  case HVC_LCON_GET: {
    /* Return up to 8 console bytes starting at offset a1, packed little-endian
     * (bytes past the end read as 0). */
    ret = 0;
    for (uint64_t k = 0; k < 8; k++) {
      uint64_t idx = a1 + k;
      if (idx < g_lcon_len)
        ret |= (uint64_t)g_lcon[idx] << (k * 8);
    }
    break;
  }
  case HVC_VM_STAT: {
    if (a1 >= NUM_VCPUS) {
      ret = (uint64_t)-1;
      break;
    }
    vcpu_t *t = &vcpus[a1];
    switch (a2) {
    case VMSTAT_ID:       ret = t->id; break;
    case VMSTAT_STATE:    ret = (uint64_t)t->state; break;
    case VMSTAT_HVC:      ret = t->hvc_count; break;
    case VMSTAT_SYSREG:   ret = t->sysreg_traps; break;
    case VMSTAT_ABORT:    ret = t->abort_count; break;
    case VMSTAT_VIRQ:     ret = t->virq_injected; break;
    case VMSTAT_MMIO:     ret = t->mmio_emulated; break;
    case VMSTAT_SWITCHES: ret = g_switch_count; break;
    case VMSTAT_WFI:      ret = g_wfi_count; break;
    default:              ret = (uint64_t)-1; break;
    }
    break;
  }
  default:
    hyp_puts("[HYP] unknown hypercall fn=");
    hyp_puthex(fn);
    hyp_puts("\n");
    ret = HVC_ERR_BADCALL;
    break;
  }

  f->x[0] = ret;
}

/* Trapped system-register access (EC=0x18), produced here by HCR_EL2.TID3 for
 * guest reads of the ID_AA64* feature registers. We emulate by returning the
 * real (optionally massaged) value, then step ELR past the trapped
 * instruction (unlike HVC, ELR points *at* it).
 *
 * ISS layout for MSR/MRS: Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10]
 *                         Rt[9:5] CRm[4:1] Direction[0] (1 = read/MRS). */
static void hyp_handle_sysreg(el2_frame_t *f) {
  uint64_t esr = MRS(esr_el2);
  uint64_t iss = esr & 0x1FFFFFFULL;
  uint64_t op0 = (iss >> 20) & 0x3;
  uint64_t op2 = (iss >> 17) & 0x7;
  uint64_t op1 = (iss >> 14) & 0x7;
  uint64_t crn = (iss >> 10) & 0xF;
  uint64_t rt = (iss >> 5) & 0x1F;
  uint64_t crm = (iss >> 1) & 0xF;
  uint64_t is_read = iss & 0x1;
  uint64_t val = 0;

  vcpus[current_vcpu].sysreg_traps++;

  /* GICv3 CPU-interface "common" registers trapped via ICH_HCR_EL2.TC. The hot
   * interrupt path (IAR/EOIR/BPR/IGRPEN, all group-1) is NOT trapped and stays
   * on the hardware virtual interface; only these init/SGI registers trap. */
  if (op0 == 3 && op1 == 0 && crn == 12 &&
      ((crm == 11 && (op2 == 5 || op2 == 6 || op2 == 7)) || /* SGI1R/ASGI1R/SGI0R */
       (crm == 11 && (op2 == 1 || op2 == 3)) ||             /* DIR / RPR          */
       (crm == 12 && op2 == 4))) {                          /* CTLR               */
    uint64_t wval = (rt == 31) ? 0 : f->x[rt];
    if (crm == 11 && op2 == 5 && !is_read) {
      /* ICC_SGI1R_EL1 write => generate an SGI. Inject a virtual SGI into the
       * targeted Linux core(s). Only Linux (vCPU 1/2) participates in SMP. */
      uint32_t intid = (uint32_t)((wval >> 24) & 0xF);
      int irm = (int)((wval >> 40) & 1);
      if (current_vcpu == 1 || current_vcpu == 2) {
        for (int aff0 = 0; aff0 < 2; aff0++) {
          int vc = 1 + aff0; /* aff0 0 => core0=vCPU1, aff0 1 => core1=vCPU2 */
          int hit = irm ? (vc != current_vcpu) : (int)((wval >> aff0) & 1);
          if (hit)
            hyp_vgic_inject_ex(vc, intid, 0 /* software SGI */);
        }
      }
    } else if (crm == 12 && op2 == 4) {
      /* ICC_CTLR_EL1: map CBPR/EOImode to ICH_VMCR_EL2; synthesize ID fields
       * (PRIbits/IDbits/A3V) from ICH_VTR_EL2 on read. */
      uint64_t vmcr = MRS(ich_vmcr_el2);
      if (is_read) {
        uint64_t vtr = MRS(ich_vtr_el2);
        uint64_t pribits = (vtr >> 29) & 0x7;
        uint64_t idbits = (vtr >> 23) & 0x7;
        uint64_t a3v = (vtr >> 21) & 0x1;
        uint64_t cbpr = (vmcr >> 4) & 1;
        uint64_t eoim = (vmcr >> 9) & 1;
        val = cbpr | (eoim << 1) | (pribits << 8) | (idbits << 11) | (a3v << 15);
      } else {
        vmcr &= ~((1ULL << 4) | (1ULL << 9));
        if (wval & 1) vmcr |= (1ULL << 4);  /* CBPR -> VCBPR */
        if (wval & 2) vmcr |= (1ULL << 9);  /* EOImode -> VEOIM */
        MSR(ich_vmcr_el2, vmcr);
      }
    }
    /* DIR/RPR/SGI0R/ASGI1R: writes ignored, reads return 0 (val stays 0). */
    if (is_read && rt != 31)
      f->x[rt] = val;
    MSR(elr_el2, MRS(elr_el2) + 4);
    return;
  }

  /* ICC_PMR_EL1 (priority mask) is also trapped by TC. Mirror it into the
   * virtual interface's VPMR field (ICH_VMCR_EL2[31:24]); a wrong VPMR would
   * block all vIRQ delivery, so this must be exact. */
  if (op0 == 3 && op1 == 0 && crn == 4 && crm == 6 && op2 == 0) {
    uint64_t vmcr = MRS(ich_vmcr_el2);
    if (is_read) {
      val = (vmcr >> 24) & 0xFF;
      if (rt != 31)
        f->x[rt] = val;
    } else {
      uint64_t wval = (rt == 31) ? 0 : f->x[rt];
      vmcr = (vmcr & ~(0xFFULL << 24)) | ((wval & 0xFF) << 24);
      MSR(ich_vmcr_el2, vmcr);
    }
    MSR(elr_el2, MRS(elr_el2) + 4);
    return;
  }

  /* Decode by (op0,op1,crn,crm,op2). Pass real values through for the ID
   * registers Fermi actually consumes; any other ID register under TID3 is
   * architecturally RES0, so returning 0 is safe. */
  if (op0 == 3 && op1 == 0 && crn == 0 && crm == 4 && op2 == 0) {
    val = MRS(id_aa64pfr0_el1); /* ID_AA64PFR0_EL1 */
    hyp_puts("[HYP] emulated guest MRS ID_AA64PFR0_EL1 -> ");
    hyp_puthex(val);
    hyp_puts("\n");
  } else if (op0 == 3 && op1 == 0 && crn == 0 && crm == 6 && op2 == 0) {
    val = MRS(id_aa64isar0_el1); /* ID_AA64ISAR0_EL1 */
  } else if (op0 == 3 && op1 == 0 && crn == 0 && crm == 7 && op2 == 0) {
    val = MRS(id_aa64mmfr0_el1); /* ID_AA64MMFR0_EL1 */
  } else {
    val = 0; /* unhandled ID register: RES0 */
  }

  if (is_read && rt != 31)
    f->x[rt] = val;

  /* Skip the trapped instruction. */
  MSR(elr_el2, MRS(elr_el2) + 4);
}

/* Minimal emulated GICv3 distributor + redistributor for the Linux guest.
 * The guest's GIC MMIO region is intentionally NOT mapped in its stage-2, so
 * accesses trap to EL2 and land here. We only need enough to satisfy Linux's
 * gic-v3 probe: report the right version/typer, complete the redistributor
 * WAKER handshake, and accept (mostly ignore) the configuration writes —
 * actual interrupt delivery for the guest's timer is done by the hypervisor
 * injecting virtual interrupts through the list registers. */
#define GICD_LO 0x08000000ULL
#define GICD_HI 0x08010000ULL
#define GICR_LO 0x080A0000ULL
#define GICR_HI 0x080E0000ULL /* 2 redistributor frames (0x20000 each) for SMP */

__attribute__((section(".hyp_tables"))) static struct {
  uint32_t gicd_ctlr;
  uint32_t gicr_ctlr;
} g_vgic;

/* Captured Linux-guest console (buffer declared with the other hypervisor
 * globals near the top of this file). */

#define PL011_LO 0x09000000ULL
#define PL011_HI 0x09001000ULL

/* Minimal emulated PL011 for the Linux guest. We only need: DR writes (capture
 * output), a flag register that says "TX ready, RX empty", and the PrimeCell /
 * peripheral ID registers so Linux's amba bus binds the pl011 driver (ttyAMA0).
 * Input (RX) always reads empty — this console is output-only by design. */
static int hyp_emulate_pl011(uint64_t ipa, int is_write, uint64_t *val) {
  uint64_t off = ipa - PL011_LO;
  if (is_write) {
    switch (off) {
    case 0x000: { /* DR: capture output byte + answer terminal cursor query */
      uint8_t c = (uint8_t)*val;
      if (g_lcon_len < LCON_SZ)
        g_lcon[g_lcon_len++] = c;
      /* The busybox line editor sends ESC[6n (cursor-position request) and
       * blocks reading the reply; without a terminal answering, it consumes
       * real input. Detect the query and reply with a cursor report so
       * line-edited input works. On the first prompt, also inject a demo
       * command (proves the interactive RX path end-to-end). */
      static const uint8_t q[4] = {0x1b, '[', '6', 'n'};
      static int m, demo_done;
      if (c == q[m]) {
        if (++m == 4) {
          m = 0;
          lrx_push_str("\x1b[1;1R"); /* cursor at row 1, col 1 */
          if (!demo_done) {
            demo_done = 1;
            /* Exercise the emulated devices through the shell: read the
             * virtio-blk signature, then bring up eth0 and ping the
             * hypervisor-emulated host (10.0.0.1) over virtio-net. */
            lrx_push_str(
                "nproc; cat /etc/motd; ifconfig eth0 10.0.0.2 up; "
                "ping -c 1 10.0.0.1; echo ALLDONE\n");
          }
          hyp_uart_rx_kick();
        }
      } else {
        m = (c == q[0]) ? 1 : 0;
      }
      break;
    }
    case 0x038: /* IMSC: interrupt mask (track RXIM) */
      g_pl011_imsc = (uint32_t)*val;
      break;
    /* CR, baud, ICR, ... accepted + ignored */
    default:
      break;
    }
    return 1;
  }
  switch (off) {
  case 0x000: /* DR: pop one RX byte (0 if none) */
    if (!lrx_empty()) {
      *val = g_lrx[g_lrx_tail];
      g_lrx_tail = (g_lrx_tail + 1) % LRX_SZ;
    } else {
      *val = 0;
    }
    break;
  case 0x018: /* FR: TXFE always; RXFE when RX FIFO empty */
    *val = (1u << 7) | (lrx_empty() ? (1u << 4) : 0);
    break;
  case 0x038: *val = g_pl011_imsc; break;                 /* IMSC            */
  case 0x03C: *val = lrx_empty() ? 0 : (PL011_RXIM | PL011_RTIM); break; /* RIS */
  case 0x040: /* MIS = RIS & IMSC */
    *val = lrx_empty() ? 0
                       : ((PL011_RXIM | PL011_RTIM) & g_pl011_imsc);
    break;
  case 0xFE0: *val = 0x11; break;       /* PeriphID0                          */
  case 0xFE4: *val = 0x10; break;       /* PeriphID1                          */
  case 0xFE8: *val = 0x14; break;       /* PeriphID2 (=> id 0x..041011)       */
  case 0xFEC: *val = 0x00; break;       /* PeriphID3                          */
  case 0xFF0: *val = 0x0D; break;       /* PCellID0                           */
  case 0xFF4: *val = 0xF0; break;       /* PCellID1                           */
  case 0xFF8: *val = 0x05; break;       /* PCellID2                           */
  case 0xFFC: *val = 0xB1; break;       /* PCellID3 (=> 0xB105F00D)           */
  default: *val = 0; break;
  }
  return 1;
}

static int hyp_emulate_gic(uint64_t ipa, int is_write, uint64_t *val) {
  if (ipa >= GICD_LO && ipa < GICD_HI) {
    uint64_t off = ipa - GICD_LO;
    if (is_write) {
      if (off == 0x000)
        g_vgic.gicd_ctlr = (uint32_t)*val;
      /* enables / priorities / routing: accepted and ignored */
    } else {
      switch (off) {
      case 0x000: *val = g_vgic.gicd_ctlr; break;          /* GICD_CTLR     */
      case 0x004: *val = (1u << 0) | (9u << 19); break;    /* TYPER: 64 INTID, IDbits=9 */
      case 0x008: *val = 0x0000043bUL; break;              /* IIDR (ARM)    */
      case 0xFFE8: *val = 0x30; break;                     /* PIDR2 => GICv3 */
      default: *val = 0; break;
      }
    }
    return 1;
  }
  if (ipa >= GICR_LO && ipa < GICR_HI) {
    uint64_t rel = ipa - GICR_LO;
    uint32_t core = (uint32_t)(rel / 0x20000); /* redistributor frame index   */
    uint64_t off = rel % 0x20000; /* RD frame at 0, SGI frame at 0x10000      */
    if (is_write) {
      if (off == 0x000)
        g_vgic.gicr_ctlr = (uint32_t)*val;
      /* WAKER and per-PPI config: accepted and ignored */
    } else {
      switch (off) {
      case 0x0000: *val = g_vgic.gicr_ctlr; break;        /* GICR_CTLR      */
      case 0x0008: /* GICR_TYPER low: ProcessorNumber[23:8], Last[4]; the      */
        /* high word (affinity, read at +0x0C or as part of a 64-bit read)     */
        /* identifies the core. core 1 is the last redistributor.             */
        *val = ((uint64_t)core << 32) | ((uint64_t)core << 8) |
               ((core == 1) ? (1ULL << 4) : 0); /* core 1 = last redistributor */
        break;
      case 0x000C: *val = core; break;                    /* TYPER high (aff) */
      case 0x0014: *val = 0; break;                       /* WAKER: awake     */
      case 0xFFE8: *val = 0x30; break;                    /* PIDR2 => GICv3   */
      default: *val = 0; break;
      }
    }
    return 1;
  }
  return 0;
}

/* Lower-EL abort. If the guest faulted trying to reach hypervisor-private
 * memory, that's our isolation boundary doing its job: report it, poison the
 * destination register on a read, and step over the access so the guest keeps
 * running. Any other abort is an unexpected (real) fault — dump and park. */
static void hyp_handle_abort(uint64_t index, el2_frame_t *frame) {
  uint64_t esr = MRS(esr_el2);
  uint64_t far = MRS(far_el2);
  uint64_t hpfar = MRS(hpfar_el2);
  uint64_t ipa_page = (hpfar >> 4) << 12; /* HPFAR[43:4] = IPA[51:12] */
  uint64_t ipa = ipa_page | (far & 0xFFF);

  uint64_t hs = (uint64_t)__hyp_start;
  uint64_t he = (uint64_t)__hyp_end;

  if (ipa_page >= (hs & ~0xFFFULL) && ipa_page < he) {
    uint64_t isv = (esr >> 24) & 1; /* instruction syndrome valid */
    uint64_t srt = (esr >> 16) & 0x1F; /* destination register      */
    uint64_t wnr = (esr >> 6) & 1;  /* write (1) vs read (0)         */

    vcpus[current_vcpu].abort_count++;
    hyp_puts("\n[HYP] ISOLATION: blocked guest ");
    hyp_puts(wnr ? "write to" : "read from");
    hyp_puts(" hyp memory IPA=");
    hyp_puthex(ipa);
    hyp_puts("\n");

    if (!wnr && isv && srt != 31)
      frame->x[srt] = 0; /* deliver a poison value for the blocked read */

    MSR(elr_el2, MRS(elr_el2) + 4); /* step past the faulting instruction */
    return;
  }

  /* Linux guest (either core): emulate trapped GIC / PL011 / virtio-mmio. */
  if ((current_vcpu == 1 || current_vcpu == 2) &&
      ((ipa >= GICD_LO && ipa < GICD_HI) || (ipa >= GICR_LO && ipa < GICR_HI) ||
       (ipa >= PL011_LO && ipa < PL011_HI) ||
       (ipa >= VIRTIO_LO && ipa < VIRTIO_HI) ||
       (ipa >= VBLK_LO && ipa < VBLK_HI) ||
       (ipa >= VNET_LO && ipa < VNET_HI))) {
    int is_uart = (ipa >= PL011_LO && ipa < PL011_HI);
    int is_virtio = (ipa >= VIRTIO_LO && ipa < VIRTIO_HI);
    int is_vblk = (ipa >= VBLK_LO && ipa < VBLK_HI);
    int is_vnet = (ipa >= VNET_LO && ipa < VNET_HI);
    vcpus[current_vcpu].mmio_emulated++;
    uint64_t isv = (esr >> 24) & 1;
    uint64_t sas = (esr >> 22) & 3;
    uint64_t srt = (esr >> 16) & 0x1F;
    uint64_t wnr = (esr >> 6) & 1;
    if (isv) {
      uint64_t v = 0;
      if (wnr) {
        v = (srt == 31) ? 0 : frame->x[srt];
        if (is_uart)
          hyp_emulate_pl011(ipa, 1, &v);
        else if (is_virtio)
          hyp_emulate_virtio(ipa, 1, &v);
        else if (is_vblk)
          hyp_emulate_vblk(ipa, 1, &v);
        else if (is_vnet)
          hyp_emulate_vnet(ipa, 1, &v);
        else
          hyp_emulate_gic(ipa, 1, &v);
      } else {
        if (is_uart)
          hyp_emulate_pl011(ipa, 0, &v);
        else if (is_virtio)
          hyp_emulate_virtio(ipa, 0, &v);
        else if (is_vblk)
          hyp_emulate_vblk(ipa, 0, &v);
        else if (is_vnet)
          hyp_emulate_vnet(ipa, 0, &v);
        else
          hyp_emulate_gic(ipa, 0, &v);
        if (sas == 0)
          v &= 0xFF;
        else if (sas == 1)
          v &= 0xFFFF;
        else if (sas == 2)
          v &= 0xFFFFFFFF;
        if (srt != 31)
          frame->x[srt] = v;
      }
    }
    MSR(elr_el2, MRS(elr_el2) + 4); /* step past the trapped instruction */
    return;
  }

  /* Linux guest hit an unhandled fault: reap it (keeping the primary guest
   * and hypervisor alive), logging where it died. */
  if (current_vcpu == 1 || current_vcpu == 2) {
    vcpus[current_vcpu].abort_count++;
    hyp_puts("\n[HYP] Linux guest unhandled abort: IPA=");
    hyp_puthex(ipa);
    hyp_puts(" ESR=");
    hyp_puthex(esr);
    hyp_puts(" ELR=");
    hyp_puthex(MRS(elr_el2));
    hyp_puts("\n");
    hyp_vcpu_exit(frame);
    return;
  }

  vcpus[current_vcpu].abort_count++;
  hyp_puts("\n[HYP] *** unexpected lower-EL abort *** vector=");
  hyp_puthex(index);
  hyp_puts(" EC=");
  hyp_puthex((esr >> ESR_EC_SHIFT) & ESR_EC_MASK);
  hyp_puts("\n      ESR_EL2=");
  hyp_puthex(esr);
  hyp_puts(" ELR_EL2=");
  hyp_puthex(MRS(elr_el2));
  hyp_puts("\n      FAR_EL2=");
  hyp_puthex(far);
  hyp_puts(" faulting IPA=");
  hyp_puthex(ipa);
  hyp_puts("\n[HYP] parking CPU for inspection.\n");
  for (;;)
    __asm__ __volatile__("wfi");
}

/* ------------------------------- vGIC -------------------------------------- */

/* Inject a hardware-linked virtual interrupt into the guest via a free list
 * register. HW=1 ties the virtual INTID to the physical one so the guest's
 * own EOI/deactivation on the virtual interface deactivates the physical
 * interrupt — no maintenance interrupt needed. Only the timer PPI is in play
 * for this guest, so a single in-flight LR is the normal case; we scan LR0/LR1
 * for robustness. */
/* Which vCPU owns a given physical INTID. The primary guest (vCPU 0) owns
 * the device/timer interrupts it programmed; extend this map as guests gain
 * their own interrupt sources. */
static int hyp_intid_owner(uint32_t intid) {
  /* CNTV (PPI 27) is per-CPU: it fires for whichever Linux core is running
   * (its CNTV_CVAL is loaded), so inject it into the current vCPU. Everything
   * else (notably the primary guest's CNTP, INTID 30) belongs to vCPU 0. */
  if (intid == 27)
    return current_vcpu;
  return 0;
}

/* Inject a virtual interrupt into `target`'s vGIC. If the target is the running
 * vCPU we write a live list register; otherwise we stash it in the target's
 * saved LR state, to be loaded when it is resumed. `hw` selects a
 * hardware-linked LR (HW=1, vINTID mapped to the same physical INTID, so the
 * guest's EOI deactivates the physical interrupt) versus a purely software /
 * emulated interrupt (HW=0, e.g. an emulated device's SPI). */
static void hyp_vgic_inject_ex(int target, uint32_t intid, int hw) {
  uint64_t lr = ((uint64_t)intid) | ICH_LR_GROUP1 | ICH_LR_STATE_PENDING;
  if (hw)
    lr |= (((uint64_t)intid) << ICH_LR_PINTID_SHIFT) | ICH_LR_HW;

  if (target == current_vcpu) {
    uint64_t lr0, lr1;
    __asm__ __volatile__("mrs %0, ich_lr0_el2" : "=r"(lr0));
    if ((lr0 >> 62) == 0) {
      __asm__ __volatile__("msr ich_lr0_el2, %0" ::"r"(lr));
      return;
    }
    __asm__ __volatile__("mrs %0, ich_lr1_el2" : "=r"(lr1));
    if ((lr1 >> 62) == 0) {
      __asm__ __volatile__("msr ich_lr1_el2, %0" ::"r"(lr));
      return;
    }
    __asm__ __volatile__("msr ich_lr0_el2, %0" ::"r"(lr)); /* fallback */
  } else {
    vcpu_t *v = &vcpus[target];
    if ((v->ich_lr[0] >> 62) == 0) {
      v->ich_lr[0] = lr;
      return;
    }
    if ((v->ich_lr[1] >> 62) == 0) {
      v->ich_lr[1] = lr;
      return;
    }
    v->ich_lr[0] = lr; /* fallback */
  }
}

/* If the Linux guest has pending input AND has enabled the PL011 RX interrupt,
 * inject the UART SPI (a software/emulated virtual interrupt) so its pl011 IRQ
 * handler drains the RX FIFO. Called each scheduling tick, so delivery is
 * robust regardless of when the guest's driver came up. */
static void hyp_uart_rx_kick(void) {
  if (!lrx_empty() && (g_pl011_imsc & (PL011_RXIM | PL011_RTIM)))
    hyp_vgic_inject_ex(1 /* Linux vCPU */, UART_SPI_INTID, 0 /* SW */);
}

/* Physical IRQ taken at EL2 (HCR_EL2.IMO). Ack on the physical CPU interface,
 * inject a hardware-linked virtual copy into the guest, then priority-drop
 * (EOImode=1 means this does not deactivate — the guest will, via the HW
 * link). The guest's existing IRQ handler runs unmodified on ICV_*. */
static void hyp_handle_irq(el2_frame_t *frame) {
  uint64_t iar;
  __asm__ __volatile__("mrs %0, icc_iar1_el1" : "=r"(iar));
  uint32_t intid = (uint32_t)(iar & 0xFFFFFF);

  if (intid >= 1020) /* 1020-1023 are special / spurious: no EOI needed */
    return;

  if (intid == HYP_TIMER_INTID) {
    /* Hypervisor scheduling tick. This interrupt is ours (not injected to a
     * guest), so fully EOI+deactivate it, re-arm the quantum, and preempt to
     * the next vCPU. */
    MSR(cnthp_cval_el2, MRS(cnthp_cval_el2) + g_quantum_ticks);
    __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"(iar)); /* prio drop  */
    __asm__ __volatile__("msr icc_dir_el1, %0" ::"r"(iar));   /* deactivate */
    hyp_uart_rx_kick(); /* deliver any pending Linux console input */
    hyp_world_switch(frame);
    return;
  }

  int owner = hyp_intid_owner(intid);
  uint64_t n = ++vcpus[owner].virq_injected;
  hyp_vgic_inject_ex(owner, intid, 1);
  __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"(iar));

  if (n <= 3) {
    hyp_puts("[HYP] injected hw vIRQ intid=");
    hyp_puthex(intid);
    hyp_puts(" -> vCPU ");
    hyp_puthex((uint64_t)owner);
    hyp_puts(" (count=");
    hyp_puthex(n);
    hyp_puts(")\n");
  }
}

void el2_dispatch(uint64_t index, el2_frame_t *frame) {
  /* Vector slot kind: 0=sync, 1=IRQ, 2=FIQ, 3=SError (within each group). */
  uint64_t kind = index & 3;

  if (kind == 1) {
    hyp_handle_irq(frame);
    return;
  }
  if (kind != 0) {
    hyp_puts("\n[HYP] unexpected EL2 exception kind=");
    hyp_puthex(kind);
    hyp_puts(" vector=");
    hyp_puthex(index);
    hyp_puts("\n");
    return;
  }

  uint64_t ec = (MRS(esr_el2) >> ESR_EC_SHIFT) & ESR_EC_MASK;

  switch (ec) {
  case EC_WFx:
    /* Idle guest hint: step past the WFI (ELR points *at* it) and yield to the
     * next runnable vCPU, so the idle slice isn't wasted halting the CPU. */
    MSR(elr_el2, MRS(elr_el2) + 4);
    g_wfi_count++;
    hyp_world_switch(frame);
    return;
  case EC_HVC64:
    hyp_handle_hvc(frame);
    return;
  case EC_SYSREG:
    hyp_handle_sysreg(frame);
    return;
  case EC_DABT_LOWER:
  case EC_IABT_LOWER:
    hyp_handle_abort(index, frame);
    return;
  default:
    hyp_puts("\n[HYP] unhandled EL2 exception: vector=");
    hyp_puthex(index);
    hyp_puts(" EC=");
    hyp_puthex(ec);
    hyp_puts(" (");
    hyp_puts(ec_name(ec));
    hyp_puts(") ELR_EL2=");
    hyp_puthex(MRS(elr_el2));
    hyp_puts("\n");
    return;
  }
}
