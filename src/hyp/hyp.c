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

extern uint8_t linux_stub[];
extern uint8_t linux_stub_end[];

/* The vCPUs and the index of the one currently running. In .hyp_tables
 * (NOLOAD); initialised explicitly in hyp_init(). */
__attribute__((section(".hyp_tables"))) static vcpu_t vcpus[NUM_VCPUS];
__attribute__((section(".hyp_tables"))) static int current_vcpu;
__attribute__((section(".hyp_tables"))) static uint64_t g_switch_count;
/* CNTHP (EL2 physical timer) scheduling tick. */
#define HYP_TIMER_INTID 26   /* PPI 26 = non-secure EL2 physical timer */
#define HYP_QUANTUM_MS 100   /* preemption time-slice */
__attribute__((section(".hyp_tables"))) static uint64_t g_quantum_ticks;

static void hyp_create_linux_guest(void);
static void hyp_tick_init(void);
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
  MSR(ich_hcr_el2, ICH_HCR_EN);              /* enable virtual CPU interface */
  __asm__ __volatile__("isb");

  /* Enable stage-2, pin EL1 to AArch64, trap guest ID-register reads
   * (TID3), and route physical IRQs to EL2 (IMO) so we can inject vIRQs. */
  MSR(hcr_el2, HCR_RW | HCR_VM | HCR_TID3 | HCR_IMO);
  __asm__ __volatile__("isb");

  /* Register the primary guest (Fermi) as vCPU 0 — its full context is
   * captured lazily on its first yield — and create the tiny guest 1. */
  vcpus[0].id = 0;
  vcpus[0].state = VCPU_RUNNING;
  vcpus[0].vttbr = (uint64_t)s2_l0; /* VMID 0 */
  current_vcpu = 0;
  hyp_create_linux_guest();
  hyp_puts("[HYP] created Linux-slot guest (vCPU 1): 1 GiB @ IPA 0x40000000\n");

  /* Start the preemptive scheduling tick (CNTHP / PPI 26). */
  hyp_tick_init();
  hyp_tick_start();
  hyp_puts("[HYP] preemptive scheduler armed (CNTHP tick)\n");

  hyp_puts("[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

/* ------------------------ world switch / vCPUs ----------------------------- */

static void hyp_save_el1(vcpu_t *v) {
  v->sp_el1 = MRS(sp_el1);
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
}

static void hyp_restore_el1(vcpu_t *v) {
  MSR(sp_el1, v->sp_el1);
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

  /* PL011 UART: IPA 0x09000000 -> phys 0x09000000 (Device). */
  uint64_t uart_idx = (0x09000000ULL % _1GB) / _2MB; /* = 72 */
  lx_l2_dev[uart_idx] =
      0x09000000ULL | S2_VALID | S2_AF | S2_AP_RW | S2_MEM_DEVICE;

  __asm__ __volatile__("dsb ish");
}

static void hyp_create_linux_guest(void) {
  hyp_build_linux_stage2();

  /* The Linux Image and DTB are staged into the guest's high RAM by QEMU's
   * generic loader (see Makefile), at IPAs 0x40200000 and 0x48000000. We just
   * enter per the arm64 boot protocol: PC = Image base, x0 = DTB, EL1h, MMU
   * off, x1..x3 = 0. */
  vcpu_t *v = &vcpus[1];
  memset(v, 0, sizeof(*v));
  v->id = 1;
  v->state = VCPU_READY;
  v->pc = LINUX_IPA_BASE + 0x200000;            /* Image entry (IPA)         */
  v->regs[0] = LINUX_IPA_BASE + 0x8000000;      /* x0 = DTB (IPA 0x48000000) */
  v->pstate = 0x3c5;                            /* EL1h, DAIF masked         */
  v->vttbr = ((uint64_t)lx_l0) | (1ULL << 48);  /* VMID 1                    */
  /* sctlr_el1 = 0 => stage-1 MMU off, as Linux's early entry expects. */
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

  /* PPI 26 -> Group1 NS, then enable it (ISENABLER is write-1-to-set). */
  uint32_t grp = mmio_read32(GICR_IGROUPR0);
  grp |= (1u << HYP_TIMER_INTID);
  mmio_write32(GICR_IGROUPR0, grp);
  mmio_write32(GICR_ISENABLER0, (1u << HYP_TIMER_INTID));
}

/* Arm CNTHP_EL2 to fire one quantum from now. */
static void hyp_tick_start(void) {
  uint64_t freq = MRS(cntfrq_el0);
  g_quantum_ticks = freq * HYP_QUANTUM_MS / 1000;
  uint64_t now = MRS(cntpct_el0);
  MSR(cnthp_cval_el2, now + g_quantum_ticks);
  MSR(cnthp_ctl_el2, 1ULL); /* enable, unmasked */
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
#define PSCI_SYSTEM_OFF 0x84000008
#define PSCI_SYSTEM_RESET 0x84000009

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

/* Minimal PSCI: report a version, and treat SYSTEM_OFF / SYSTEM_RESET from a
 * guest as "this VM is done" — reap it. Returns nonzero if handled. */
static int hyp_handle_psci(el2_frame_t *f, uint64_t fn) {
  switch (fn) {
  case PSCI_VERSION_FN:
    f->x[0] = 0x00010001; /* PSCI v1.1 */
    return 1;
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
    case VMSTAT_SWITCHES: ret = g_switch_count; break;
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

/* Lower-EL abort. If the guest faulted trying to reach hypervisor-private
 * memory, that's our isolation boundary doing its job: report it, poison the
 * destination register on a read, and step over the access so the guest keeps
 * running. Any other abort is an unexpected (real) fault — dump and park. */
static void hyp_handle_abort(uint64_t index, el2_frame_t *frame) {
  vcpus[current_vcpu].abort_count++;

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
  (void)intid;
  return 0;
}

/* Inject a hardware-linked virtual interrupt into `target`'s vGIC. If the
 * target is the running vCPU we write a live list register; otherwise we
 * stash it in the target's saved LR state, to be loaded when it is resumed.
 * HW=1 ties the vINTID to the physical one so the guest's own EOI deactivates
 * the physical interrupt. */
static void hyp_vgic_inject(int target, uint32_t intid) {
  uint64_t lr = ((uint64_t)intid) |
                (((uint64_t)intid) << ICH_LR_PINTID_SHIFT) |
                ICH_LR_GROUP1 | ICH_LR_HW | ICH_LR_STATE_PENDING;

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
    hyp_world_switch(frame);
    return;
  }

  int owner = hyp_intid_owner(intid);
  uint64_t n = ++vcpus[owner].virq_injected;
  hyp_vgic_inject(owner, intid);
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
