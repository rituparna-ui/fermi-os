#include "el3.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * el3.c — EL3 Root-world Secure Monitor (E0a)
 *
 * Runs at EL3 with the MMU off. For now it just brings up the console and
 * reports the world state before boot.S ERETs to the Non-secure world. The
 * GPT/GPC setup (E0b) and the Realm-world launch (E1) build on this.
 *
 * EL3 owns SCR_EL3 (world selection / SMC routing) and, with FEAT_RME, the
 * Granule Protection Check registers GPCCR_EL3 / GPTBR_EL3. We read a few here
 * purely to show they are reachable from Root world.
 * --------------------------------------------------------------------------- */

/* Dedicated EL3 stack (monitor-private; lives in the reserved .hyp_tables
 * region so it is neither visible to the Non-secure world's stage-2 nor
 * reused by the guest PMM). Size must match EL3_STACK_SIZE in boot.S. */
__attribute__((aligned(16), section(".hyp_tables"))) uint8_t el3_stack[8192];

#define MRS(reg)                                                               \
  ({                                                                           \
    uint64_t _v;                                                               \
    __asm__ __volatile__("mrs %0, " #reg : "=r"(_v));                          \
    _v;                                                                        \
  })

#define MSR(reg, val)                                                          \
  do {                                                                         \
    uint64_t _v = (val);                                                       \
    __asm__ __volatile__("msr " #reg ", %0" ::"r"(_v));                        \
  } while (0)

/* --- FEAT_RME Granule Protection Table (E0b) ----------------------------- *
 * The GPT tags every physical granule with a Granule Protection Information
 * (GPI) value naming which PAS may access it. The hardware GPC checks every
 * access against it. We use a single-level (L0) GPT with each entry a "block
 * descriptor" covering one L0GPTSZ region. With L0GPTSZ=1GB and PPS=1TB that
 * is 1024 entries (8 KiB). All entries start as GPI_ANY (all worlds), so
 * enabling GPC changes nothing yet — delegation (flipping entries to Realm)
 * comes in E2. Field layout / encodings follow the Arm ARM (as used by TF-A).
 *
 * GPCCR_EL3 = S3_6_C2_C1_6, GPTBR_EL3 = S3_6_C2_C1_4. */
#define GPI_NO_ACCESS 0x0
#define GPI_SECURE 0x8
#define GPI_NS 0x9
#define GPI_ROOT 0xA
#define GPI_REALM 0xB
#define GPI_ANY 0xF
#define GPT_L0_BLK_TYPE 0x1 /* block descriptor; GPI in bits[7:4] */
#define GPT_L0_BLK(gpi) (((uint64_t)(gpi) << 4) | GPT_L0_BLK_TYPE)

#define GPCCR_PPS_SHIFT 0   /* protected PA size (PARange-style)   */
#define GPCCR_IRGN_SHIFT 8  /* GPT walk inner cacheability         */
#define GPCCR_ORGN_SHIFT 10 /* GPT walk outer cacheability         */
#define GPCCR_SH_SHIFT 12   /* GPT walk shareability               */
#define GPCCR_PGS_SHIFT 14  /* physical granule size (00=4K)       */
#define GPCCR_GPC_SHIFT 16  /* GPC enable                          */
#define GPCCR_L0GPTSZ_SHIFT 20 /* size each L0 entry covers (0=1GB)*/

#define GPCCR_PPS_1TB 2  /* 40-bit PA */
#define GPCCR_PGS_4K 0
#define GPCCR_WB_WA 1    /* IRGN/ORGN write-back write-allocate */
#define GPCCR_SH_INNER 3
#define GPCCR_L0GPTSZ_1GB 0

#define GPT_L0_ENTRIES 1024 /* 1 TiB / 1 GiB */
__attribute__((aligned(0x4000), section(".hyp_tables"))) static uint64_t gpt_l0[GPT_L0_ENTRIES];

static void el3_build_gpt(void) {
  for (int i = 0; i < GPT_L0_ENTRIES; i++)
    gpt_l0[i] = GPT_L0_BLK(GPI_ANY); /* all worlds may access, for now */
  __asm__ __volatile__("dsb sy");
}

static void el3_enable_gpc(void) {
  el3_build_gpt();
  /* GPTBR_EL3 holds the L0 GPT base as PA[51:12]. */
  MSR(S3_6_C2_C1_4, ((uint64_t)gpt_l0) >> 12);
  uint64_t gpccr = ((uint64_t)GPCCR_PPS_1TB << GPCCR_PPS_SHIFT) |
                   ((uint64_t)GPCCR_WB_WA << GPCCR_IRGN_SHIFT) |
                   ((uint64_t)GPCCR_WB_WA << GPCCR_ORGN_SHIFT) |
                   ((uint64_t)GPCCR_SH_INNER << GPCCR_SH_SHIFT) |
                   ((uint64_t)GPCCR_PGS_4K << GPCCR_PGS_SHIFT) |
                   ((uint64_t)GPCCR_L0GPTSZ_1GB << GPCCR_L0GPTSZ_SHIFT) |
                   (1ULL << GPCCR_GPC_SHIFT); /* enable GPC */
  MSR(S3_6_C2_C1_6, gpccr);
  __asm__ __volatile__("dsb sy; isb");
  /* Invalidate any stale GPT info cached by the GPC. */
  __asm__ __volatile__("tlbi paallos; dsb sy; isb");
}

/* PL011 UART0 on QEMU virt. With secure=on this UART is reachable from Root. */
#define UART0 0x09000000UL
#define U_DR (UART0 + 0x00)
#define U_FR (UART0 + 0x18)
#define U_IBRD (UART0 + 0x24)
#define U_FBRD (UART0 + 0x28)
#define U_LCRH (UART0 + 0x2C)
#define U_CR (UART0 + 0x30)
#define U_ICR (UART0 + 0x44)
#define U_FR_TXFF (1u << 5)

static inline void w32(uint64_t a, uint32_t v) {
  *(volatile uint32_t *)a = v;
}
static inline uint32_t r32(uint64_t a) {
  return *(volatile uint32_t *)a;
}

/* QEMU 11's PL011 drops writes while the UART is disabled, so the monitor must
 * enable it before printing (older QEMU is lenient, but we do it always). */
static void el3_uart_init(void) {
  w32(U_CR, 0);
  w32(U_ICR, 0x7FF);
  w32(U_IBRD, 13);
  w32(U_FBRD, 2);
  w32(U_LCRH, (1 << 4) | (1 << 5) | (1 << 6));
  w32(U_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

static void el3_putc(char c) {
  while (r32(U_FR) & U_FR_TXFF) {
  }
  w32(U_DR, (uint32_t)c);
}

static void el3_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      el3_putc('\r');
    el3_putc(*s++);
  }
}

static void el3_puthex(uint64_t v) {
  el3_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uint64_t nib = (v >> i) & 0xF;
    el3_putc((char)(nib < 10 ? '0' + nib : 'a' + (nib - 10)));
  }
}

/* --- EL3 synchronous-exception (SMC) handling (E1) ----------------------- */
#define ESR_EC_SHIFT 26
#define EC_SMC64 0x17
#define KVA_OFFSET 0xFFFF000000000000ULL

extern char el2_path[]; /* boot.S label (Non-secure EL2 entry) */

/* Reconfigure EL3 so the vector's ERET drops into Non-secure EL2 (the host).
 * el3.c runs at EL3 with the MMU off and -fno-pic, so &el2_path is already a
 * physical address (PC-relative adrp) — no VA offset to subtract. */
static void el3_enter_ns(void) {
  MSR(elr_el3, (uint64_t)el2_path);                       /* physical entry */
  MSR(spsr_el3, 0x3c9);                                   /* EL2h, DAIF masked */
  MSR(scr_el3, (1ULL << 0) | (1ULL << 8) | (1ULL << 10)); /* NS|HCE|RW */
  __asm__ __volatile__("isb");
}

void el3_sync_handler(el3_frame_t *f) {
  uint64_t esr = MRS(esr_el3);
  uint64_t ec = (esr >> ESR_EC_SHIFT) & 0x3f;
  if (ec == EC_SMC64) {
    uint64_t fn = f->x[0];
    if (fn == RMM_BOOT_COMPLETE) {
      el3_puts("[EL3] RMM_BOOT_COMPLETE; entering Non-secure world\n");
      el3_enter_ns(); /* el3_common's eret will now land in NS-EL2 */
      return;
    }
    el3_puts("[EL3] unknown SMC fn=");
    el3_puthex(fn);
    el3_puts("\n");
    f->x[0] = (uint64_t)-1;
    return;
  }
  el3_puts("[EL3] *** unexpected EL3 exception EC=");
  el3_puthex(ec);
  el3_puts(" ESR=");
  el3_puthex(esr);
  el3_puts(" ELR=");
  el3_puthex(MRS(elr_el3));
  el3_puts(" — parking\n");
  for (;;)
    __asm__ __volatile__("wfi");
}

void el3_init(void) {
  el3_uart_init();
  el3_puts("\n[EL3] Root-world Secure Monitor online\n");

  /* Install the EL3 vector table so SMCs (from the RMM, and later the host)
   * trap here. Address is physical (MMU off), which is what VBAR_EL3 needs. */
  extern char el3_vector_table[];
  MSR(vbar_el3, (uint64_t)el3_vector_table);

  uint64_t el = (MRS(CurrentEL) >> 2) & 0x3;
  el3_puts("[EL3] CurrentEL = ");
  el3_puthex(el);
  el3_puts(" (Root world)\n");

  /* Prove the RME Granule-Protection registers are reachable from Root.
   * GPCCR_EL3 = S3_6_C2_C1_6, GPTBR_EL3 = S3_6_C2_C1_4. */
  uint64_t gpccr = MRS(S3_6_C2_C1_6);
  el3_puts("[EL3] GPCCR_EL3 = ");
  el3_puthex(gpccr);
  el3_puts(" (GPC not yet enabled)\n");

  /* Build an all-access GPT and turn on Granule Protection Checks. From here
   * every physical access is checked by hardware; delegation (E2) will flip
   * specific granules to the Realm PAS to lock the host out. */
  el3_enable_gpc();
  el3_puts("[EL3] GPC enabled, GPCCR_EL3 = ");
  el3_puthex(MRS(S3_6_C2_C1_6));
  el3_puts(" (GPT: 1 TiB, all-access)\n");

  el3_puts("[EL3] launching RMM in the Realm world (Realm-EL2)...\n");
}
