#include "rmm.h"
#include "rmm/rmi.h" /* RMI_* host ABI */
#include "mm/mmu/mmu.h" /* _1GB, _512GB */
#include "mm/pmm/pmm.h" /* MEM_START, MEM_SIZE */
#include "uart/uart.h"  /* UART_BASE / UART_DR / UART_FR */

/* ---------------------------------------------------------------------------
 * rmm.c — EL2 Realm Management Monitor core (forked from Fermi hyp M3)
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

/* The single guest's vCPU control block. In .hyp_tables (NOLOAD), so it is
 * neither zeroed by the guest's zero_bss nor reused by the guest PMM; we
 * initialise its fields explicitly in rmm_init(). */
__attribute__((section(".hyp_tables"))) static vcpu_t g_vcpu;

/* --- self-contained PL011 output (no driver state, safe pre-uart_init) --- */
static void rmm_putc(char c) {
  volatile uint32_t *fr = (volatile uint32_t *)UART_FR;
  volatile uint32_t *dr = (volatile uint32_t *)UART_DR;
  while (*fr & UART_FR_TXFF) {
  }
  *dr = (uint32_t)c;
}

static void rmm_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      rmm_putc('\r');
    rmm_putc(*s++);
  }
}

static void rmm_puthex(uint64_t v) {
  rmm_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uint64_t nib = (v >> i) & 0xF;
    rmm_putc((char)(nib < 10 ? '0' + nib : 'a' + (nib - 10)));
  }
}

/* Build the IPA==PA identity map with 1 GiB blocks.
 *   - RAM [MEM_START, MEM_START+MEM_SIZE)            -> Normal WB
 *   - everything else (GIC, UART, PCI ECAM/MMIO)     -> Device-nGnRnE
 * Pointers are physical here (PC-relative, MMU off), which is exactly what
 * the descriptors and VTTBR_EL2 must contain. */
static void rmm_build_stage2(void) {
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

void rmm_init(void) {
  rmm_puts("\n[RMM] Fermi RMM online at EL2\n");

  /* Initialise the guest vCPU control block (NOLOAD memory => not zeroed). */
  g_vcpu.id = 0;
  g_vcpu.rmi_count = 0;
  g_vcpu.sysreg_traps = 0;
  g_vcpu.abort_count = 0;

  /* Sanity: confirm we really are at EL2. */
  uint64_t el = (MRS(CurrentEL) >> 2) & 0x3;
  rmm_puts("[RMM] CurrentEL = ");
  rmm_puthex(el);
  rmm_puts("\n");

  /* Generic timer: zero the virtual offset and let EL1/EL0 use the physical
   * counter and timer registers directly (Fermi drives the timer from EL1). */
  MSR(cntvoff_el2, 0);
  {
    uint64_t cnthctl = MRS(cnthctl_el2);
    cnthctl |= (CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN);
    MSR(cnthctl_el2, cnthctl);
  }

  /* Stage-2 translation tables (with the hypervisor's own RAM unmapped). */
  rmm_build_stage2();
  rmm_puts("[RMM] isolated hyp region [");
  rmm_puthex((uint64_t)__hyp_start);
  rmm_puts(", ");
  rmm_puthex((uint64_t)__hyp_end);
  rmm_puts(") from guest stage-2\n");

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

  /* Enable stage-2, pin EL1 to AArch64, and trap guest ID-register reads
   * (HCR_EL2.TID3) so we can emulate the CPU feature view. */
  MSR(hcr_el2, HCR_RW | HCR_VM | HCR_TID3);
  __asm__ __volatile__("isb");

  rmm_puts("[RMM] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest...\n");
}

/* ------------------------------- traps ------------------------------------ */

static const char *ec_name(uint64_t ec) {
  switch (ec) {
  case EC_HVC64:
    return "HVC (RMI/RSI call)";
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

/* RMI command: FID in x0, args in x1..x3, status/result back in x0.
 * ELR_EL2 already points past the HVC, so no PC adjustment is needed. This is
 * the Normal-world host driving the monitor. */
static void rmm_handle_rmi(el2_frame_t *f) {
  uint64_t cmd = f->x[0];
  uint64_t a1 = f->x[1];
  uint64_t ret;

  g_vcpu.rmi_count++;

  switch (cmd) {
  case RMI_VERSION:
    ret = RMI_ABI_VERSION;
    break;
  case RMI_FEATURES:
    /* No optional features advertised yet. */
    ret = 0;
    break;
  case RMI_PUTC:
    rmm_putc((char)a1);
    ret = RMI_SUCCESS;
    break;
  case RMI_PING:
    ret = a1 + 1;
    break;
  case RMI_MONITOR_INFO:
    ret = g_vcpu.rmi_count;
    break;
  case RMI_MONITOR_BASE:
    /* Introspection probe: expose the monitor-private base IPA so the host
     * can attempt — and be denied by stage-2 — an access to RMM memory. */
    ret = (uint64_t)__hyp_start;
    break;
  default:
    rmm_puts("[RMM] unknown RMI command=");
    rmm_puthex(cmd);
    rmm_puts("\n");
    ret = RMI_ERROR_NOT_SUPPORTED;
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
static void rmm_handle_sysreg(el2_frame_t *f) {
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

  g_vcpu.sysreg_traps++;

  /* Decode by (op0,op1,crn,crm,op2). Pass real values through for the ID
   * registers Fermi actually consumes; any other ID register under TID3 is
   * architecturally RES0, so returning 0 is safe. */
  if (op0 == 3 && op1 == 0 && crn == 0 && crm == 4 && op2 == 0) {
    val = MRS(id_aa64pfr0_el1); /* ID_AA64PFR0_EL1 */
    rmm_puts("[RMM] emulated guest MRS ID_AA64PFR0_EL1 -> ");
    rmm_puthex(val);
    rmm_puts("\n");
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
static void rmm_handle_abort(uint64_t index, el2_frame_t *frame) {
  g_vcpu.abort_count++;

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

    rmm_puts("\n[RMM] ISOLATION: blocked guest ");
    rmm_puts(wnr ? "write to" : "read from");
    rmm_puts(" hyp memory IPA=");
    rmm_puthex(ipa);
    rmm_puts("\n");

    if (!wnr && isv && srt != 31)
      frame->x[srt] = 0; /* deliver a poison value for the blocked read */

    MSR(elr_el2, MRS(elr_el2) + 4); /* step past the faulting instruction */
    return;
  }

  rmm_puts("\n[RMM] *** unexpected lower-EL abort *** vector=");
  rmm_puthex(index);
  rmm_puts(" EC=");
  rmm_puthex((esr >> ESR_EC_SHIFT) & ESR_EC_MASK);
  rmm_puts("\n      ESR_EL2=");
  rmm_puthex(esr);
  rmm_puts(" ELR_EL2=");
  rmm_puthex(MRS(elr_el2));
  rmm_puts("\n      FAR_EL2=");
  rmm_puthex(far);
  rmm_puts(" faulting IPA=");
  rmm_puthex(ipa);
  rmm_puts("\n[RMM] parking CPU for inspection.\n");
  for (;;)
    __asm__ __volatile__("wfi");
}

void el2_dispatch(uint64_t index, el2_frame_t *frame) {
  uint64_t ec = (MRS(esr_el2) >> ESR_EC_SHIFT) & ESR_EC_MASK;

  switch (ec) {
  case EC_HVC64:
    rmm_handle_rmi(frame);
    return;
  case EC_SYSREG:
    rmm_handle_sysreg(frame);
    return;
  case EC_DABT_LOWER:
  case EC_IABT_LOWER:
    rmm_handle_abort(index, frame);
    return;
  default:
    rmm_puts("\n[RMM] unhandled EL2 exception: vector=");
    rmm_puthex(index);
    rmm_puts(" EC=");
    rmm_puthex(ec);
    rmm_puts(" (");
    rmm_puts(ec_name(ec));
    rmm_puts(") ELR_EL2=");
    rmm_puthex(MRS(elr_el2));
    rmm_puts("\n");
    return;
  }
}
