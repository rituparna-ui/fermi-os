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

  /* Root node. */
  begin_node(&e, "");
  prop_u32(&e, "#address-cells", 2);
  prop_u32(&e, "#size-cells", 2);
  prop(&e, "compatible", "linux,dummy-virt", 17);

  /* /memory@<base> */
  begin_node(&e, "memory@40000000");
  prop(&e, "device_type", "memory", 7);
  prop_reg2(&e, "reg", mem_base, mem_size);
  end_node(&e);

  /* /pl011@<uart> */
  begin_node(&e, "pl011@9000000");
  prop(&e, "compatible", "arm,pl011", 10);
  prop_reg2(&e, "reg", uart_base, 0x1000);
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
