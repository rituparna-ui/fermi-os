#include "fdt.h"

/* DTB / FDT format (Devicetree Spec): all multi-byte fields are big-endian.
 * Layout we emit:
 *   [fdt_header]
 *   [memory reservation block]  (one terminator entry: 0,0)
 *   [structure block]           (token stream, 4-byte aligned)
 *   [strings block]             (NUL-terminated property names)
 */

struct fdt_header {
  uint32_t magic;
  uint32_t totalsize;
  uint32_t off_dt_struct;
  uint32_t off_dt_strings;
  uint32_t off_mem_rsvmap;
  uint32_t version;
  uint32_t last_comp_version;
  uint32_t boot_cpuid_phys;
  uint32_t size_dt_strings;
  uint32_t size_dt_struct;
};

#define FDT_BEGIN_NODE 0x1U
#define FDT_END_NODE   0x2U
#define FDT_PROP       0x3U
#define FDT_NOP        0x4U
#define FDT_END        0x9U

static uint32_t bswap32(uint32_t v) {
  return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) |
         ((v >> 24) & 0xFF);
}
static uint64_t bswap64(uint64_t v) {
  return ((uint64_t)bswap32((uint32_t)v) << 32) | bswap32((uint32_t)(v >> 32));
}

/* --- little emit cursor over the structure + strings blocks --- */
typedef struct {
  uint8_t *base;
  uint32_t cap;
  uint32_t soff;  /* struct cursor (from base)              */
  uint32_t str0;  /* strings block start offset (from base) */
  uint32_t scur;  /* strings cursor (from base)             */
  int      overflow;
} emit_t;

static void s_u32(emit_t *e, uint32_t v) {
  if (e->soff + 4 > e->str0) { e->overflow = 1; return; }
  uint32_t be = bswap32(v);
  __builtin_memcpy(e->base + e->soff, &be, 4);
  e->soff += 4;
}

static void s_align(emit_t *e) {
  while (e->soff & 3) {
    if (e->soff >= e->str0) { e->overflow = 1; return; }
    e->base[e->soff++] = 0;
  }
}

/* Intern a property name into the strings block; return its offset. */
static uint32_t str_intern(emit_t *e, const char *name) {
  uint32_t off = e->scur - e->str0;
  const char *p = name;
  do {
    if (e->scur >= e->cap) { e->overflow = 1; return 0; }
    e->base[e->scur++] = (uint8_t)*p;
  } while (*p++);
  return off;
}

static void begin_node(emit_t *e, const char *name) {
  s_u32(e, FDT_BEGIN_NODE);
  const char *p = name;
  do {
    if (e->soff >= e->str0) { e->overflow = 1; return; }
    e->base[e->soff++] = (uint8_t)*p;
  } while (*p++);
  s_align(e);
}

static void end_node(emit_t *e) { s_u32(e, FDT_END_NODE); }

/* Emit a property whose value is `len` raw bytes at `val`. */
static void prop(emit_t *e, const char *name, const void *val, uint32_t len) {
  s_u32(e, FDT_PROP);
  s_u32(e, len);
  s_u32(e, str_intern(e, name));
  for (uint32_t i = 0; i < len; i++) {
    if (e->soff >= e->str0) { e->overflow = 1; return; }
    e->base[e->soff++] = ((const uint8_t *)val)[i];
  }
  s_align(e);
}

static void prop_u32(emit_t *e, const char *name, uint32_t v) {
  uint32_t be = bswap32(v);
  prop(e, name, &be, 4);
}

/* A "reg" of one (address,size) pair with #address-cells=2,#size-cells=2. */
static void prop_reg2(emit_t *e, const char *name, uint64_t addr, uint64_t sz) {
  uint64_t cells[2] = {bswap64(addr), bswap64(sz)};
  prop(e, name, cells, sizeof(cells));
}

/* An empty property (e.g. "interrupt-controller;"). */
static void prop_empty(emit_t *e, const char *name) {
  prop(e, name, (const void *)0, 0);
}

/* A big-endian u32 array property. */
static void prop_cells(emit_t *e, const char *name, const uint32_t *cells,
                       int n) {
  uint32_t tmp[24];
  for (int i = 0; i < n && i < 24; i++) {
    tmp[i] = bswap32(cells[i]);
  }
  prop(e, name, tmp, (uint32_t)n * 4);
}

uint32_t fdt_build(void *buf, uint32_t cap, uint64_t mem_base,
                   uint64_t mem_size, uint64_t uart_base) {
  if (cap < 256) {
    return 0;
  }
  uint8_t *b = (uint8_t *)buf;
  for (uint32_t i = 0; i < cap; i++) {
    b[i] = 0;
  }

  uint32_t hdr_sz = sizeof(struct fdt_header);
  uint32_t rsv_off = (hdr_sz + 7) & ~7U; /* mem-rsvmap is 8-byte aligned */
  uint32_t struct_off = rsv_off + 16;    /* one terminator rsv entry (0,0) */
  struct_off = (struct_off + 3) & ~3U;

  /* Reserve the back third of the buffer for the strings block. */
  uint32_t strings_off = struct_off + (cap - struct_off) * 2 / 3;
  strings_off &= ~3U;

  emit_t e = {.base = b, .cap = cap, .soff = struct_off,
              .str0 = strings_off, .scur = strings_off, .overflow = 0};

  /* GICv3 phandle, referenced by interrupt-producing nodes. */
  const uint32_t GIC_PHANDLE = 1;
  /* Fixed-clock phandle, referenced by the PL011 as its uartclk + apb_pclk.
   * The AMBA bus probe REQUIRES an "apb_pclk" clock or it aborts, so the
   * PL011 only registers ttyAMA0 (letting init open /dev/console) when this
   * clock is wired. Matches QEMU virt's own binding. */
  const uint32_t CLK_PHANDLE = 2;

  /* Root node. */
  begin_node(&e, "");
  prop_u32(&e, "#address-cells", 2);
  prop_u32(&e, "#size-cells", 2);
  prop_u32(&e, "interrupt-parent", GIC_PHANDLE);
  prop(&e, "compatible", "linux,dummy-virt", 17);

  /* /chosen — boot arguments + console. A real OS reads stdout-path here. */
  begin_node(&e, "chosen");
  prop(&e, "bootargs", "console=ttyAMA0 earlycon", 25);
  prop(&e, "stdout-path", "/pl011@9000000", 15);
  end_node(&e);

  /* /memory@<base> */
  begin_node(&e, "memory@40000000");
  prop(&e, "device_type", "memory", 7);
  prop_reg2(&e, "reg", mem_base, mem_size);
  end_node(&e);

  /* /psci — power/SMP services (an OS uses CPU_ON to start secondaries). */
  begin_node(&e, "psci");
  prop(&e, "compatible", "arm,psci-1.0", 13);
  prop(&e, "method", "hvc", 4);
  end_node(&e);

  /* /cpus with two PSCI-bring-up cores (matches M23 SMP). */
  begin_node(&e, "cpus");
  prop_u32(&e, "#address-cells", 1);
  prop_u32(&e, "#size-cells", 0);
  begin_node(&e, "cpu@0");
  prop(&e, "device_type", "cpu", 4);
  prop(&e, "compatible", "arm,cortex-a72", 15);
  prop_u32(&e, "reg", 0);
  prop(&e, "enable-method", "psci", 5);
  end_node(&e);
  begin_node(&e, "cpu@1");
  prop(&e, "device_type", "cpu", 4);
  prop(&e, "compatible", "arm,cortex-a72", 15);
  prop_u32(&e, "reg", 1);
  prop(&e, "enable-method", "psci", 5);
  end_node(&e);
  end_node(&e); /* cpus */

  /* /timer — ARM generic timer. interrupts: <type ppi flags> triplets for
   * secure-phys(13), non-secure-phys(14), virt(11), hyp(10); GIC_PPI=1,
   * level-low active = 0x108. */
  begin_node(&e, "timer");
  prop(&e, "compatible", "arm,armv8-timer", 16);
  {
    uint32_t it[12] = {1, 13, 0x108, 1, 14, 0x108, 1, 11, 0x108, 1, 10, 0x108};
    prop_cells(&e, "interrupts", it, 12);
  }
  prop_empty(&e, "always-on");
  end_node(&e);

  /* /intc — GICv3: distributor @0x08000000 (64K) + redistributor @0x080A0000
   * (128K). This is the interrupt-parent referenced above. */
  begin_node(&e, "intc@8000000");
  prop(&e, "compatible", "arm,gic-v3", 11);
  prop_u32(&e, "#interrupt-cells", 3);
  prop_empty(&e, "interrupt-controller");
  prop_u32(&e, "#address-cells", 2);
  prop_u32(&e, "#size-cells", 2);
  prop_empty(&e, "ranges");
  {
    /* two reg ranges: GICD then GICR (each addr,size as 2+2 cells). */
    uint64_t regs[4] = {bswap64(0x08000000ULL), bswap64(0x10000ULL),
                        bswap64(0x080A0000ULL), bswap64(0x20000ULL)};
    prop(&e, "reg", regs, sizeof(regs));
  }
  prop_u32(&e, "phandle", GIC_PHANDLE);
  end_node(&e);

  /* /apb-pclk — a 24 MHz fixed-clock the PL011 references. The common-clock
   * framework (CONFIG_COMMON_CLK) instantiates "fixed-clock" nodes via
   * CLK_OF_DECLARE, so this resolves before the AMBA bus probes the UART. */
  begin_node(&e, "apb-pclk");
  prop(&e, "compatible", "fixed-clock", 12);
  prop_u32(&e, "#clock-cells", 0);
  prop_u32(&e, "clock-frequency", 24000000);
  prop_u32(&e, "phandle", CLK_PHANDLE);
  end_node(&e);

  /* /pl011@<uart> — a proper AMBA PrimeCell so the amba-pl011 driver binds and
   * registers ttyAMA0 (which lets userspace init open /dev/console).
   *   - compatible MUST include "arm,primecell" or of_amba_device_create makes
   *     a plain platform_device the pl011 driver never matches.
   *   - clocks/clock-names supply the mandatory "apb_pclk" (+ "uartclk").
   * The GIC SPI interrupt is declared for completeness; console TX is polled,
   * so it works even though the hypervisor doesn't route this SPI to the guest. */
  begin_node(&e, "pl011@9000000");
  /* String-list compatible: "arm,pl011\0arm,primecell\0". */
  prop(&e, "compatible", "arm,pl011\0arm,primecell", 24);
  prop_reg2(&e, "reg", uart_base, 0x1000);
  {
    uint32_t it[3] = {0, 1, 0x4}; /* GIC_SPI=0, intid 1, level-high */
    prop_cells(&e, "interrupts", it, 3);
  }
  {
    uint32_t clks[2] = {CLK_PHANDLE, CLK_PHANDLE}; /* uartclk, apb_pclk */
    prop_cells(&e, "clocks", clks, 2);
  }
  prop(&e, "clock-names", "uartclk\0apb_pclk", 17); /* both strings NUL-term */
  end_node(&e);

  end_node(&e); /* root */
  s_u32(&e, FDT_END);

  if (e.overflow) {
    return 0;
  }

  uint32_t size_dt_struct = e.soff - struct_off;
  uint32_t size_dt_strings = e.scur - strings_off;
  uint32_t totalsize = e.scur;

  /* Memory reservation terminator entry (address=0, size=0). */
  uint64_t z = 0;
  __builtin_memcpy(b + rsv_off, &z, 8);
  __builtin_memcpy(b + rsv_off + 8, &z, 8);

  struct fdt_header h = {
      .magic = bswap32(FDT_MAGIC),
      .totalsize = bswap32(totalsize),
      .off_dt_struct = bswap32(struct_off),
      .off_dt_strings = bswap32(strings_off),
      .off_mem_rsvmap = bswap32(rsv_off),
      .version = bswap32(17),
      .last_comp_version = bswap32(16),
      .boot_cpuid_phys = 0,
      .size_dt_strings = bswap32(size_dt_strings),
      .size_dt_struct = bswap32(size_dt_struct),
  };
  __builtin_memcpy(b, &h, sizeof(h));
  return totalsize;
}

uint32_t fdt_check(const void *buf) {
  const struct fdt_header *h = (const struct fdt_header *)buf;
  if (bswap32(h->magic) != FDT_MAGIC) {
    return 0;
  }
  uint32_t ts = bswap32(h->totalsize);
  if (ts < sizeof(struct fdt_header) || ts > (1U << 20)) {
    return 0;
  }
  return ts;
}
