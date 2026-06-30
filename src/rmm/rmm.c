#include "rmm.h"
#include "rmm/measure.h" /* sha256, rim_extend */
#include "rmm/rmi.h" /* RMI_* host ABI */
#include "rmm/rsi.h" /* RSI_* realm ABI */
#include "mm/mmu/mmu.h" /* _1GB, _512GB */
#include "mm/pmm/pmm.h" /* MEM_START, MEM_SIZE */
#include "strings/strings.h" /* memset, memcpy */
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

/* ---------------------------------------------------------------------------
 * Granule state machine (R2)
 *
 * A "granule" is one 4 KiB physical page. In Arm CCA every page of delegable
 * RAM has a state the RMM tracks in the Granule State Table; the host moves a
 * page UNDELEGATED -> DELEGATED via RMI before the RMM can repurpose it as a
 * Realm Descriptor / RTT / REC / DATA page. Delegation is enforced by stage-2:
 * a DELEGATED granule is unmapped from the Normal world's IPA space, so the
 * host physically cannot read or write it while the monitor owns it.
 *
 * This minimal table tracks a bounded set of granules; entries are created
 * lazily the first time the host references a page. The table itself lives in
 * RMM-private .hyp_tables, so the host can neither see nor forge it. The
 * RD/RTT/REC/DATA sub-states arrive in later milestones. */
typedef enum {
  GRANULE_UNDELEGATED = 0, /* owned by the Normal-world host               */
  GRANULE_DELEGATED = 1,   /* owned by the RMM; removed from host stage-2  */
  GRANULE_RD = 2,          /* holds a Realm Descriptor                     */
  GRANULE_RTT = 3,         /* a Realm Translation Table (realm stage-2)    */
  GRANULE_DATA = 4,        /* a data page mapped into a realm's IPA space  */
  GRANULE_REC = 5,         /* a Realm Execution Context (saved vCPU state) */
} granule_state_t;

#define MAX_GRANULES 64
typedef struct {
  uint64_t pa;            /* page-aligned physical address (== IPA)         */
  granule_state_t state;
} granule_t;
__attribute__((section(".hyp_tables"))) static granule_t g_granules[MAX_GRANULES];
__attribute__((section(".hyp_tables"))) static uint64_t g_granule_count;

/* ---------------------------------------------------------------------------
 * Realm Descriptor + RTT (R3)
 *
 * A Realm is created from a DELEGATED granule that becomes its Realm
 * Descriptor (RD) — the monitor's control block for that realm. A second
 * DELEGATED granule becomes the root of the realm's RTT (Realm Translation
 * Table): a *separate* stage-2 tree describing the realm's own IPA space,
 * independent of the Normal world's. Pages are mapped into the realm with
 * RMI_RTT_MAP, which walks the RTT (allocating intermediate tables) and
 * installs a leaf descriptor.
 *
 * For this milestone the intermediate RTT levels are allocated from an
 * RMM-private pool; a fuller implementation has the host delegate a granule
 * per level via RMI_RTT_CREATE. The realm's stage-2 is built here but only
 * becomes the live VTTBR when we run a REC under it (R4). */
typedef enum {
  REALM_NEW = 0,    /* created, RTT being populated         */
  REALM_ACTIVE = 1, /* runnable (set once a REC exists, R4)  */
} realm_state_t;

typedef struct {
  int valid;
  uint64_t rd_pa;       /* the RD granule — also the realm handle           */
  uint64_t vmid;        /* stage-2 VMID for this realm                      */
  uint64_t rtt_base_pa; /* granule backing the realm's stage-2 L0 table     */
  uint64_t *rtt_l0;     /* == rtt_base_pa (physical; EL2 MMU off)           */
  realm_state_t state;
  uint64_t mapped_pages;
  uint8_t rim[32]; /* Realm Initial Measurement (hash chain) */
} realm_t;
#define MAX_REALMS 4
__attribute__((section(".hyp_tables"))) static realm_t g_realms[MAX_REALMS];

/* Pool of intermediate RTT tables (realm stage-2 L1/L2/L3), RMM-private. */
#define RTT_POOL_SIZE 16
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t rtt_pool[RTT_POOL_SIZE][512];
__attribute__((section(".hyp_tables"))) static uint64_t rtt_pool_used;

/* ---------------------------------------------------------------------------
 * Realm Execution Context (REC) + world switch (R4)
 *
 * A REC is the saved CPU state of one realm vCPU, created from a DELEGATED
 * granule. RMI_REC_ENTER world-switches from the Normal world into the realm:
 * it saves the host context, loads the REC, points VTTBR_EL2 at the realm's
 * RTT (with the realm VMID), and erets to EL1 — now running the realm's code
 * under the realm's private stage-2. When the realm traps back (HVC/abort),
 * the RMM saves the REC, restores the host, and RMI_REC_ENTER returns the
 * exit reason to the host. */
typedef struct {
  int valid;
  uint64_t rec_pa;  /* the REC granule (handle)                              */
  realm_t *realm;   /* owning realm                                          */
  vcpu_t ctx;       /* saved realm CPU state                                 */
  uint64_t exits;   /* number of realm exits observed                        */
} rec_t;
#define MAX_RECS 4
__attribute__((section(".hyp_tables"))) static rec_t g_recs[MAX_RECS];

/* The Normal-world host runs as g_vcpu (declared above); its context is saved
 * there while a realm runs. g_running_rec is non-NULL exactly when a realm is
 * executing, so el2_dispatch can tell a host RMI call apart from a realm
 * exit. */
__attribute__((section(".hyp_tables"))) static rec_t *g_running_rec;

/* Realm payload blob (linked into the kernel image; the RMM copies its bytes
 * into a realm DATA granule via RMI_DATA_CREATE). */
extern uint8_t realm_payload[];
extern uint8_t realm_payload_end[];

/* Pool of L3 tables used to split a 2 MiB stage-2 block down to 4 KiB pages
 * on demand, so an arbitrary host page can be unmapped (delegated). Each pool
 * entry backs one 2 MiB block. */
#define MAX_L3_POOL 8
__attribute__((aligned(4096), section(".hyp_tables"))) static uint64_t s2_l3_pool[MAX_L3_POOL][512];
__attribute__((section(".hyp_tables"))) static uint64_t s2_l3_block[MAX_L3_POOL]; /* 2 MiB base each entry backs; 0 = free */
__attribute__((section(".hyp_tables"))) static uint64_t s2_l3_used;

/* Captured in rmm_build_stage2(): the single 1 GiB region that is L2-split to
 * 2 MiB blocks, and the 2 MiB block already split to 4 KiB for the monitor's
 * own hole (which reuses the static s2_l3_split table). */
__attribute__((section(".hyp_tables"))) static uint64_t g_split_gb;
__attribute__((section(".hyp_tables"))) static uint64_t g_mon_mb_base;

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

/* Print an N-byte hash as a bare hex string. */
static void rmm_puthash(const uint8_t *h, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t hi = h[i] >> 4, lo = h[i] & 0xF;
    rmm_putc((char)(hi < 10 ? '0' + hi : 'a' + hi - 10));
    rmm_putc((char)(lo < 10 ? '0' + lo : 'a' + lo - 10));
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

  /* Remember the split region so on-demand granule delegation (R2) can split
   * additional 2 MiB blocks within it and reuse s2_l3_split for the monitor's
   * own block. */
  g_split_gb = gb_idx;
  g_mon_mb_base = mb_base;

  __asm__ __volatile__("dsb ish");
}

void rmm_init(void) {
  rmm_puts("\n[RMM] Fermi RMM online at EL2\n");

  /* Initialise the host context (NOLOAD memory => not zeroed). g_vcpu is the
   * Normal world (VMID 0); its vttbr is set once stage-2 tables are built. */
  g_vcpu.id = 0;
  g_vcpu.rmi_count = 0;
  g_vcpu.sysreg_traps = 0;
  g_vcpu.abort_count = 0;
  g_running_rec = 0;

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

/* ----------------------- granule / stage-2 (R2) --------------------------- */

/* Broadcast stage-2 TLB invalidation for the current VMID. */
static void s2_tlbi_all(void) {
  __asm__ __volatile__("dsb ish; tlbi vmalls12e1is; dsb ish; isb");
}

/* Return the L3 (4 KiB) stage-2 table covering `pa`, splitting its 2 MiB
 * block on demand. Only the single L2-split 1 GiB region is supported (that's
 * where RAM and the host live). Returns 0 if `pa` is outside that region or
 * the L3 pool is exhausted. */
static uint64_t *s2_l3_table_for(uint64_t pa) {
  uint64_t gb = pa / _1GB;
  if (gb != g_split_gb)
    return 0; /* outside the split arena */
  uint64_t region_base = gb * _1GB;
  uint64_t mb_idx = (pa - region_base) / _2MB;
  uint64_t mb_base = region_base + mb_idx * _2MB;

  if (mb_base == g_mon_mb_base)
    return s2_l3_split; /* monitor's block: already split at boot */

  for (uint64_t i = 0; i < s2_l3_used; i++)
    if (s2_l3_block[i] == mb_base)
      return s2_l3_pool[i]; /* this block was split earlier */

  if (s2_l3_used >= MAX_L3_POOL)
    return 0; /* pool exhausted */

  /* Split this 2 MiB block: populate a fresh L3 with identity 4 KiB pages,
   * then splice it into the L2 table in place of the 2 MiB block. */
  uint64_t *l3 = s2_l3_pool[s2_l3_used];
  for (uint64_t p = 0; p < 512; p++) {
    uint64_t ppa = mb_base + p * 4096ULL;
    l3[p] = ppa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
  }
  s2_l2_split[mb_idx] = ((uint64_t)l3) | S2_TABLE | S2_VALID;
  s2_l3_block[s2_l3_used] = mb_base;
  s2_l3_used++;
  __asm__ __volatile__("dsb ish");
  s2_tlbi_all();
  return l3;
}

/* Map (mapped=1) or unmap (mapped=0) a single 4 KiB page in the host's
 * stage-2 view. Returns 0 on success, -1 if the page is unsupported. */
static int s2_set_page(uint64_t pa, int mapped) {
  uint64_t *l3 = s2_l3_table_for(pa);
  if (!l3)
    return -1;
  uint64_t region_base = g_split_gb * _1GB;
  uint64_t mb_base = region_base + ((pa - region_base) / _2MB) * _2MB;
  uint64_t p = (pa - mb_base) / 4096ULL;
  l3[p] = mapped ? (pa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW |
                    S2_MEM_NORMAL)
                 : 0;
  s2_tlbi_all();
  return 0;
}

/* Look up (or lazily create) the granule entry for a page-aligned PA. */
static granule_t *granule_find(uint64_t pa) {
  for (uint64_t i = 0; i < g_granule_count; i++)
    if (g_granules[i].pa == pa)
      return &g_granules[i];
  return 0;
}

static granule_t *granule_intern(uint64_t pa) {
  granule_t *g = granule_find(pa);
  if (g)
    return g;
  if (g_granule_count >= MAX_GRANULES)
    return 0;
  g = &g_granules[g_granule_count++];
  g->pa = pa;
  g->state = GRANULE_UNDELEGATED;
  return g;
}

/* RMI_GRANULE_DELEGATE: the host gives a page to the RMM. We unmap it from the
 * host's stage-2 (so the Normal world loses all access) and mark it
 * DELEGATED. */
static uint64_t rmi_granule_delegate(uint64_t pa) {
  pa &= ~0xFFFULL;
  if (pa >= (uint64_t)__hyp_start && pa < (uint64_t)__hyp_end)
    return RMI_ERROR_INPUT; /* monitor memory is never delegable */
  granule_t *g = granule_intern(pa);
  if (!g || g->state != GRANULE_UNDELEGATED)
    return RMI_ERROR_INPUT;
  if (s2_set_page(pa, 0) != 0)
    return RMI_ERROR_INPUT;
  g->state = GRANULE_DELEGATED;
  rmm_puts("[RMM] granule ");
  rmm_puthex(pa);
  rmm_puts(" DELEGATED (unmapped from host stage-2)\n");
  return RMI_SUCCESS;
}

/* RMI_GRANULE_UNDELEGATE: the host reclaims a page. The RMM scrubs it first
 * (so no Realm/monitor data leaks back to the Normal world) and remaps it. */
static uint64_t rmi_granule_undelegate(uint64_t pa) {
  pa &= ~0xFFFULL;
  granule_t *g = granule_find(pa);
  if (!g || g->state != GRANULE_DELEGATED)
    return RMI_ERROR_INPUT;
  /* EL2 runs with its MMU off, so the RMM addresses the page physically and
   * can scrub it before handing it back. */
  volatile uint64_t *p = (volatile uint64_t *)pa;
  for (uint64_t i = 0; i < 4096 / sizeof(uint64_t); i++)
    p[i] = 0;
  if (s2_set_page(pa, 1) != 0)
    return RMI_ERROR_INPUT;
  g->state = GRANULE_UNDELEGATED;
  rmm_puts("[RMM] granule ");
  rmm_puthex(pa);
  rmm_puts(" UNDELEGATED (scrubbed + remapped to host)\n");
  return RMI_SUCCESS;
}

/* ----------------------- realm / RTT (R3) --------------------------------- */

static realm_t *realm_find(uint64_t rd_pa) {
  for (int i = 0; i < MAX_REALMS; i++)
    if (g_realms[i].valid && g_realms[i].rd_pa == rd_pa)
      return &g_realms[i];
  return 0;
}

static uint64_t *rtt_alloc(void) {
  if (rtt_pool_used >= RTT_POOL_SIZE)
    return 0;
  uint64_t *t = rtt_pool[rtt_pool_used++];
  for (int i = 0; i < 512; i++)
    t[i] = 0;
  return t;
}

/* Descend one stage-2 level, allocating the next-level RTT if absent. */
static uint64_t *rtt_next(uint64_t *table, uint64_t idx) {
  if (!(table[idx] & S2_VALID)) {
    uint64_t *t = rtt_alloc();
    if (!t)
      return 0;
    table[idx] = ((uint64_t)t) | S2_TABLE | S2_VALID;
  }
  return (uint64_t *)(table[idx] & 0x0000FFFFFFFFF000ULL);
}

/* RMI_REALM_CREATE: consume rd_pa as the Realm Descriptor and rtt_base_pa as
 * the root of the realm's stage-2. Both must be DELEGATED. */
static uint64_t rmi_realm_create(uint64_t rd_pa, uint64_t rtt_base_pa) {
  rd_pa &= ~0xFFFULL;
  rtt_base_pa &= ~0xFFFULL;
  granule_t *grd = granule_find(rd_pa);
  granule_t *grtt = granule_find(rtt_base_pa);
  if (!grd || grd->state != GRANULE_DELEGATED)
    return RMI_ERROR_INPUT;
  if (!grtt || grtt->state != GRANULE_DELEGATED || rtt_base_pa == rd_pa)
    return RMI_ERROR_INPUT;

  int slot = -1;
  for (int i = 0; i < MAX_REALMS; i++)
    if (!g_realms[i].valid) {
      slot = i;
      break;
    }
  if (slot < 0)
    return RMI_ERROR_REALM;

  realm_t *r = &g_realms[slot];
  r->valid = 1;
  r->rd_pa = rd_pa;
  r->vmid = (uint64_t)slot + 1; /* VMID 0 is the Normal world */
  r->rtt_base_pa = rtt_base_pa;
  r->rtt_l0 = (uint64_t *)rtt_base_pa; /* EL2 MMU off: physical == pointer */
  for (int i = 0; i < 512; i++)
    r->rtt_l0[i] = 0; /* empty realm IPA space */
  r->state = REALM_NEW;
  r->mapped_pages = 0;
  memset(r->rim, 0, sizeof(r->rim)); /* RIM starts empty; extended by DATA_CREATE */

  grd->state = GRANULE_RD;
  grtt->state = GRANULE_RTT;

  rmm_puts("[RMM] REALM_CREATE rd=");
  rmm_puthex(rd_pa);
  rmm_puts(" rtt=");
  rmm_puthex(rtt_base_pa);
  rmm_puts(" vmid=");
  rmm_puthex(r->vmid);
  rmm_puts("\n");
  return RMI_SUCCESS;
}

/* Walk the realm's RTT to `ipa` (allocating intermediate levels) and install a
 * leaf mapping to `pa`. Returns 0 on success, -1 if the RTT pool is empty. */
static int realm_map_page(realm_t *r, uint64_t ipa, uint64_t pa) {
  uint64_t *l1 = rtt_next(r->rtt_l0, (ipa >> 39) & 0x1FF);
  if (!l1)
    return -1;
  uint64_t *l2 = rtt_next(l1, (ipa >> 30) & 0x1FF);
  if (!l2)
    return -1;
  uint64_t *l3 = rtt_next(l2, (ipa >> 21) & 0x1FF);
  if (!l3)
    return -1;
  l3[(ipa >> 12) & 0x1FF] =
      pa | S2_TABLE | S2_AF | S2_SH_INNER | S2_AP_RW | S2_MEM_NORMAL;
  __asm__ __volatile__("dsb ish");
  return 0;
}

/* RMI_RTT_MAP: map data_pa into realm rd_pa at realm IPA `ipa`. data_pa must
 * be DELEGATED; it transitions to DATA. Intermediate RTT levels are allocated
 * from the RMM-private pool. */
static uint64_t rmi_rtt_map(uint64_t rd_pa, uint64_t ipa, uint64_t data_pa) {
  rd_pa &= ~0xFFFULL;
  ipa &= ~0xFFFULL;
  data_pa &= ~0xFFFULL;
  realm_t *r = realm_find(rd_pa);
  if (!r)
    return RMI_ERROR_REALM;
  granule_t *gd = granule_find(data_pa);
  if (!gd || gd->state != GRANULE_DELEGATED)
    return RMI_ERROR_INPUT;

  if (realm_map_page(r, ipa, data_pa) != 0)
    return RMI_ERROR_RTT;

  gd->state = GRANULE_DATA;
  r->mapped_pages++;

  rmm_puts("[RMM] RTT_MAP realm=");
  rmm_puthex(rd_pa);
  rmm_puts(" ipa=");
  rmm_puthex(ipa);
  rmm_puts(" -> pa=");
  rmm_puthex(data_pa);
  rmm_puts("\n");
  return RMI_SUCCESS;
}

/* RMI_RTT_READ_ENTRY (introspection): software-walk the realm's RTT and
 * return the PA mapped at `ipa`, or 0 if unmapped. */
static uint64_t rmi_rtt_read_entry(uint64_t rd_pa, uint64_t ipa) {
  rd_pa &= ~0xFFFULL;
  realm_t *r = realm_find(rd_pa);
  if (!r)
    return 0;
  uint64_t *t = r->rtt_l0;
  uint64_t shifts[4] = {39, 30, 21, 12};
  for (int lvl = 0; lvl < 3; lvl++) {
    uint64_t e = t[(ipa >> shifts[lvl]) & 0x1FF];
    if (!(e & S2_VALID))
      return 0;
    t = (uint64_t *)(e & 0x0000FFFFFFFFF000ULL);
  }
  uint64_t leaf = t[(ipa >> 12) & 0x1FF];
  if (!(leaf & S2_VALID))
    return 0;
  return (leaf & 0x0000FFFFFFFFF000ULL) | (ipa & 0xFFF);
}

/* ----------------------- REC / world switch (R4) -------------------------- */

static void rmm_save_el1(vcpu_t *v) {
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

static void rmm_restore_el1(vcpu_t *v) {
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

static rec_t *rec_find(uint64_t rec_pa) {
  for (int i = 0; i < MAX_RECS; i++)
    if (g_recs[i].valid && g_recs[i].rec_pa == rec_pa)
      return &g_recs[i];
  return 0;
}

/* RMI_DATA_CREATE: copy a page of host content (src_pa) into a DELEGATED data
 * granule and map it into the realm at `ipa`. This is how a realm's initial
 * memory (e.g. its code) is loaded; granule -> DATA. (Measurement of the
 * loaded page is added in R5.) */
static uint64_t rmi_data_create(uint64_t rd_pa, uint64_t data_pa, uint64_t ipa,
                                uint64_t src_pa) {
  rd_pa &= ~0xFFFULL;
  data_pa &= ~0xFFFULL;
  ipa &= ~0xFFFULL;
  realm_t *r = realm_find(rd_pa);
  if (!r)
    return RMI_ERROR_REALM;
  granule_t *gd = granule_find(data_pa);
  if (!gd || gd->state != GRANULE_DELEGATED)
    return RMI_ERROR_INPUT;
  /* EL2 runs MMU-off: address both pages physically. */
  memcpy((void *)data_pa, (void *)src_pa, 4096);
  if (realm_map_page(r, ipa, data_pa) != 0)
    return RMI_ERROR_RTT;
  gd->state = GRANULE_DATA;
  r->mapped_pages++;
  /* Measure the loaded page into the realm's RIM (binds content + IPA). */
  rim_extend(r->rim, ipa, (void *)data_pa, 4096);
  rmm_puts("[RMM] DATA_CREATE realm=");
  rmm_puthex(rd_pa);
  rmm_puts(" ipa=");
  rmm_puthex(ipa);
  rmm_puts(" <- src=");
  rmm_puthex(src_pa);
  rmm_puts("\n[RMM]   RIM now ");
  rmm_puthash(r->rim, 32);
  rmm_puts("\n");
  return RMI_SUCCESS;
}

/* RMI_REC_CREATE: turn a DELEGATED granule into a Realm Execution Context with
 * its entry PC set to `entry_ipa`. The REC runs at EL1 with its stage-1 MMU
 * off (EL1 sysregs zeroed), so it sees its own RTT-mapped IPA space. */
static uint64_t rmi_rec_create(uint64_t rd_pa, uint64_t rec_pa,
                               uint64_t entry_ipa) {
  rd_pa &= ~0xFFFULL;
  rec_pa &= ~0xFFFULL;
  realm_t *r = realm_find(rd_pa);
  if (!r)
    return RMI_ERROR_REALM;
  granule_t *gr = granule_find(rec_pa);
  if (!gr || gr->state != GRANULE_DELEGATED)
    return RMI_ERROR_INPUT;

  int slot = -1;
  for (int i = 0; i < MAX_RECS; i++)
    if (!g_recs[i].valid) {
      slot = i;
      break;
    }
  if (slot < 0)
    return RMI_ERROR_REC;

  rec_t *rec = &g_recs[slot];
  memset(rec, 0, sizeof(*rec));
  rec->valid = 1;
  rec->rec_pa = rec_pa;
  rec->realm = r;
  rec->ctx.pc = entry_ipa;
  rec->ctx.pstate = 0x3c5; /* EL1h, DAIF masked */
  rec->ctx.vttbr = r->rtt_base_pa | (r->vmid << 48);
  /* EL1 sysregs left 0 => realm runs with its stage-1 MMU off. */

  gr->state = GRANULE_REC;
  r->state = REALM_ACTIVE;
  rmm_puts("[RMM] REC_CREATE rec=");
  rmm_puthex(rec_pa);
  rmm_puts(" entry_ipa=");
  rmm_puthex(entry_ipa);
  rmm_puts(" vmid=");
  rmm_puthex(r->vmid);
  rmm_puts("\n");
  return RMI_SUCCESS;
}

/* RMI_REC_ENTER: world-switch from the Normal world into the realm. Saves the
 * host context into g_vcpu, loads the REC into the trap frame + EL1 sysregs,
 * and points VTTBR_EL2 at the realm's stage-2. el2_common then restores the
 * (now realm) frame and erets into the realm. Does NOT write f->x[0] — the
 * realm's x0 must survive into the realm. */
static void rmi_rec_enter(el2_frame_t *f, uint64_t rec_pa) {
  rec_t *rec = rec_find(rec_pa & ~0xFFFULL);
  if (!rec) {
    f->x[0] = RMI_ERROR_REC;
    return;
  }

  /* Save host context (resume point is right after the REC_ENTER hvc). */
  for (int i = 0; i < 31; i++)
    g_vcpu.regs[i] = f->x[i];
  g_vcpu.pc = MRS(elr_el2);
  g_vcpu.pstate = MRS(spsr_el2);
  g_vcpu.vttbr = MRS(vttbr_el2);
  rmm_save_el1(&g_vcpu);

  /* Load the realm REC. */
  for (int i = 0; i < 31; i++)
    f->x[i] = rec->ctx.regs[i];
  MSR(elr_el2, rec->ctx.pc);
  MSR(spsr_el2, rec->ctx.pstate);
  MSR(vttbr_el2, rec->ctx.vttbr);
  rmm_restore_el1(&rec->ctx);
  g_running_rec = rec;

  rmm_puts("[RMM] REC_ENTER -> entering realm vmid=");
  rmm_puthex(rec->realm->vmid);
  rmm_puts("\n");
}

/* Service an RSI call from the realm. Returns 1 if handled in place (the realm
 * should be resumed immediately) or 0 if the call requires returning to the
 * Normal-world host. The result is written into the realm's x0 either way. */
static int rsi_service(rec_t *rec, el2_frame_t *f, uint64_t *host_reason) {
  uint64_t fn = f->x[0];
  switch (fn) {
  case RSI_VERSION:
    f->x[0] = RSI_ABI_VERSION;
    return 1;
  case RSI_REALM_CONFIG:
    f->x[0] = rec->realm->vmid; /* minimal config: the realm's VMID */
    return 1;
  case RSI_PUTC:
    rmm_putc((char)f->x[1]); /* realm paravirt console via the RMM */
    f->x[0] = 0;
    return 1;
  case RSI_ATTESTATION_TOKEN: {
    /* Produce an attestation token bound to the realm's measured identity and
     * a caller-supplied challenge: token = SHA-256(RIM || challenge_le64). A
     * real CCA token is a signed COSE/CBOR structure; this captures the
     * essential binding. Returns the token's low 64 bits in x0. */
    uint64_t challenge = f->x[1];
    uint8_t token[32];
    sha256_ctx sc;
    sha256_init(&sc);
    sha256_update(&sc, rec->realm->rim, 32);
    uint8_t chb[8];
    for (int i = 0; i < 8; i++)
      chb[i] = (uint8_t)(challenge >> (8 * i));
    sha256_update(&sc, chb, 8);
    sha256_final(&sc, token);
    rmm_puts("[RMM] ATTESTATION vmid=");
    rmm_puthex(rec->realm->vmid);
    rmm_puts(" challenge=");
    rmm_puthex(challenge);
    rmm_puts("\n[RMM]   RIM   ");
    rmm_puthash(rec->realm->rim, 32);
    rmm_puts("\n[RMM]   token ");
    rmm_puthash(token, 32);
    rmm_puts("\n");
    uint64_t lo = 0;
    for (int i = 0; i < 8; i++)
      lo |= (uint64_t)token[i] << (8 * i);
    f->x[0] = lo;
    return 1;
  }
  case RSI_HOST_CALL:
    rmm_puts("[RMM] realm RSI_HOST_CALL arg=");
    rmm_puthex(f->x[1]);
    rmm_puts(" -> exit to host\n");
    *host_reason = REC_EXIT_HOST_CALL;
    return 0;
  case RSI_EXIT:
    rmm_puts("[RMM] realm RSI_EXIT -> realm finished\n");
    *host_reason = REC_EXIT_DONE;
    return 0;
  default:
    rmm_puts("[RMM] unknown RSI fn=");
    rmm_puthex(fn);
    rmm_puts("\n");
    f->x[0] = (uint64_t)-1;
    return 1; /* resume the realm with an error result */
  }
}

/* A trap arrived while a realm was running. If it is an RSI call we can serve
 * in place, handle it and resume the realm (no host involvement). Otherwise
 * (RSI_HOST_CALL / RSI_EXIT, or a fault) save the REC, restore the host, and
 * return an exit reason from RMI_REC_ENTER. */
static void rmm_handle_realm_trap(el2_frame_t *f) {
  rec_t *rec = g_running_rec;
  uint64_t ec = (MRS(esr_el2) >> ESR_EC_SHIFT) & ESR_EC_MASK;
  uint64_t reason = REC_EXIT_ABORT;

  if (ec == EC_HVC64) {
    if (rsi_service(rec, f, &reason)) {
      /* Serviced in place: leave the realm context live (VTTBR, EL1 sysregs,
       * ELR_EL2 already past the HVC) and just resume — el2_common restores
       * the frame (with the RSI result in x0) and erets back into the realm. */
      return;
    }
    /* else: fall through to the world switch back to the host. */
  } else {
    rmm_puts("[RMM] realm fault EC=");
    rmm_puthex(ec);
    rmm_puts(" FAR_EL2=");
    rmm_puthex(MRS(far_el2));
    rmm_puts("\n");
  }

  /* World switch out: save the REC, restore the host, return the reason. */
  for (int i = 0; i < 31; i++)
    rec->ctx.regs[i] = f->x[i];
  rec->ctx.pc = MRS(elr_el2);
  rec->ctx.pstate = MRS(spsr_el2);
  rec->ctx.vttbr = MRS(vttbr_el2);
  rmm_save_el1(&rec->ctx);
  rec->exits++;

  for (int i = 0; i < 31; i++)
    f->x[i] = g_vcpu.regs[i];
  MSR(elr_el2, g_vcpu.pc);
  MSR(spsr_el2, g_vcpu.pstate);
  MSR(vttbr_el2, g_vcpu.vttbr);
  rmm_restore_el1(&g_vcpu);
  g_running_rec = 0;

  f->x[0] = reason; /* RMI_REC_ENTER's result to the host */
}

/* RMI_REALM_RIM (introspection): copy the realm's 32-byte RIM into a host
 * buffer. In real CCA the RIM is delivered inside the signed attestation
 * token; here we expose it so the host/verifier can recompute and compare. */
static uint64_t rmi_realm_rim(uint64_t rd_pa, uint64_t out_pa) {
  realm_t *r = realm_find(rd_pa & ~0xFFFULL);
  if (!r)
    return RMI_ERROR_REALM;
  memcpy((void *)out_pa, r->rim, 32);
  return RMI_SUCCESS;
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
  case RMI_GRANULE_DELEGATE:
    ret = rmi_granule_delegate(a1);
    break;
  case RMI_GRANULE_UNDELEGATE:
    ret = rmi_granule_undelegate(a1);
    break;
  case RMI_REALM_CREATE:
    ret = rmi_realm_create(a1, f->x[2]);
    break;
  case RMI_RTT_MAP:
    ret = rmi_rtt_map(a1, f->x[2], f->x[3]);
    break;
  case RMI_RTT_READ_ENTRY:
    ret = rmi_rtt_read_entry(a1, f->x[2]);
    break;
  case RMI_REALM_RIM:
    ret = rmi_realm_rim(a1, f->x[2]);
    break;
  case RMI_DATA_CREATE:
    ret = rmi_data_create(a1, f->x[2], f->x[3], f->x[4]);
    break;
  case RMI_REC_CREATE:
    ret = rmi_rec_create(a1, f->x[2], f->x[3]);
    break;
  case RMI_REC_ENTER:
    /* Performs the world switch and loads the realm frame; must not clobber
     * the realm's x0 with a status, so return immediately. */
    rmi_rec_enter(f, a1);
    return;
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

  int in_monitor = (ipa_page >= (hs & ~0xFFFULL) && ipa_page < he);
  granule_t *g = granule_find(ipa_page);
  int delegated = (g && g->state != GRANULE_UNDELEGATED);

  if (in_monitor || delegated) {
    uint64_t isv = (esr >> 24) & 1; /* instruction syndrome valid */
    uint64_t srt = (esr >> 16) & 0x1F; /* destination register      */
    uint64_t wnr = (esr >> 6) & 1;  /* write (1) vs read (0)         */

    rmm_puts("\n[RMM] ISOLATION: blocked host ");
    rmm_puts(wnr ? "write to" : "read from");
    rmm_puts(in_monitor ? " monitor memory IPA=" : " delegated granule IPA=");
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
  /* If a realm is currently running, any trap is a realm exit, not a host
   * RMI call. Hand it to the world-switch-out path. */
  if (g_running_rec) {
    rmm_handle_realm_trap(frame);
    return;
  }

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
