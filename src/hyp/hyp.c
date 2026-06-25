#include "hyp.h"
#include "mm/mmu/mmu.h" /* _1GB, _512GB */
#include "mm/pmm/pmm.h" /* MEM_START, MEM_SIZE */
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

  __asm__ __volatile__("dsb ish");
}

void hyp_init(void) {
  hyp_puts("\n[HYP] Fermi hypervisor online at EL2\n");

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

  /* Stage-2 translation tables. */
  hyp_build_stage2();

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

  /* Enable stage-2 and pin EL1 to AArch64. From this point the EL1 guest's
   * physical accesses are IPA->PA translated by the tables above. */
  MSR(hcr_el2, HCR_RW | HCR_VM);
  __asm__ __volatile__("isb");

  hyp_puts("[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

/* ------------------------------- traps ------------------------------------ */

static const char *ec_name(uint64_t ec) {
  switch (ec) {
  case EC_HVC64:
    return "HVC (hypercall)";
  case EC_DABT_LOWER:
    return "data abort (stage-2)";
  case EC_IABT_LOWER:
    return "instruction abort (stage-2)";
  default:
    return "other";
  }
}

void el2_dispatch(uint64_t index, el2_frame_t *frame) {
  uint64_t esr = MRS(esr_el2);
  uint64_t elr = MRS(elr_el2);
  uint64_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

  hyp_puts("\n[HYP] *** EL2 trap *** vector=");
  hyp_puthex(index);
  hyp_puts(" EC=");
  hyp_puthex(ec);
  hyp_puts(" (");
  hyp_puts(ec_name(ec));
  hyp_puts(")\n      ESR_EL2=");
  hyp_puthex(esr);
  hyp_puts(" ELR_EL2=");
  hyp_puthex(elr);
  hyp_puts("\n");

  if (ec == EC_HVC64) {
    /* HVC #imm: the 16-bit immediate is in ESR_EL2[15:0]. ELR_EL2 already
     * points to the instruction *after* the HVC, so a plain eret resumes
     * the guest correctly. This is our Milestone-1 proof that guest->EL2
     * world transitions work. */
    hyp_puts("      hypercall imm=");
    hyp_puthex(esr & 0xFFFF);
    hyp_puts(", returning to guest\n\n");
    (void)frame;
    return;
  }

  /* Any other trap at this milestone is unexpected. For a lower-EL abort,
   * dump the full stage-2 context ONCE then park the CPU, so the log is
   * readable instead of an endless re-fault spam loop. */
  static int dumped = 0;
  if (!dumped) {
    dumped = 1;
    uint64_t far = MRS(far_el2);
    uint64_t hpfar = MRS(hpfar_el2);
    uint64_t ipa = (hpfar >> 4) << 12; /* HPFAR[43:4] = IPA[51:12] */
    hyp_puts("      FAR_EL2=");
    hyp_puthex(far);
    hyp_puts(" HPFAR_EL2=");
    hyp_puthex(hpfar);
    hyp_puts("\n      faulting IPA=");
    hyp_puthex(ipa);
    hyp_puts("\n      VTTBR_EL2=");
    hyp_puthex(MRS(vttbr_el2));
    hyp_puts(" VTCR_EL2=");
    hyp_puthex(MRS(vtcr_el2));
    hyp_puts("\n      HCR_EL2=");
    hyp_puthex(MRS(hcr_el2));
    hyp_puts("\n      &s2_l0=");
    hyp_puthex((uint64_t)s2_l0);
    hyp_puts(" s2_l0[0]=");
    hyp_puthex(s2_l0[0]);
    hyp_puts("\n      &s2_l1_low=");
    hyp_puthex((uint64_t)s2_l1_low);
    hyp_puts(" s2_l1_low[1]=");
    hyp_puthex(s2_l1_low[1]);
    hyp_puts("\n[HYP] parking CPU for inspection.\n");
    for (;;)
      __asm__ __volatile__("wfi");
  }
  hyp_puts("[HYP] unhandled EL2 exception (continuing)\n\n");
}
