/* Foreign mini-guest (milestone 22) — a standalone AArch64 EL1 program that
 * shares NO code with FermiOS. It is entered (from start.S) with the DTB
 * pointer, parses the flattened device tree to discover its UART and RAM, and
 * prints what it found through the discovered UART. This proves the hypervisor
 * can boot a generic, DTB-driven guest, not just FermiOS.
 *
 * Self-contained: only <stdint.h>, no other includes. */

#include <stdint.h>

#define FDT_BEGIN_NODE 0x1U
#define FDT_END_NODE   0x2U
#define FDT_PROP       0x3U
#define FDT_NOP        0x4U
#define FDT_END        0x9U

struct fdt_header {
  uint32_t magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
  uint32_t version, last_comp_version, boot_cpuid_phys;
  uint32_t size_dt_strings, size_dt_struct;
};

static uint32_t be32(uint32_t v) {
  return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) |
         ((v >> 24) & 0xFF);
}
static uint64_t be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
static int str_pfx(const char *s, const char *pfx) {
  while (*pfx) { if (*s++ != *pfx++) return 0; }
  return 1;
}

/* Discovered hardware. */
static volatile uint32_t *g_uart;    /* PL011 base (set from DTB)        */
static uint64_t g_mem_base, g_mem_size;

/* --- UART (PL011) — only what we need to print. --- */
static void uart_putc(char c) {
  /* FR (offset 0x18) bit5 = TXFF; spin while full. */
  while (g_uart[0x18 / 4] & (1u << 5)) { }
  g_uart[0] = (uint32_t)c;            /* DR */
}
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
static void uart_hex(uint64_t v) {
  uart_puts("0x");
  int started = 0;
  for (int i = 60; i >= 0; i -= 4) {
    uint8_t n = (v >> i) & 0xF;
    if (n || started || i == 0) {
      uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
      started = 1;
    }
  }
}

/* Walk the FDT structure block. We only need two leaf facts: the reg of the
 * node whose name starts with "pl011" (UART base) and the reg of the "memory"
 * node (base,size). Property names are looked up in the strings block. */
static void parse_dtb(const uint8_t *dtb) {
  const struct fdt_header *h = (const struct fdt_header *)dtb;
  const uint8_t *strs = dtb + be32(h->off_dt_strings);
  const uint8_t *p = dtb + be32(h->off_dt_struct);
  const uint8_t *end = p + be32(h->size_dt_struct);

  char cur[40];               /* current node name */
  cur[0] = 0;

  while (p < end) {
    uint32_t tok = be32(*(const uint32_t *)p);
    p += 4;
    if (tok == FDT_BEGIN_NODE) {
      int i = 0;
      while (p[i] && i < (int)sizeof(cur) - 1) { cur[i] = (char)p[i]; i++; }
      cur[i] = 0;
      p += i + 1;
      p = (const uint8_t *)(((uintptr_t)p + 3) & ~3UL);
    } else if (tok == FDT_PROP) {
      uint32_t len = be32(*(const uint32_t *)p); p += 4;
      uint32_t noff = be32(*(const uint32_t *)p); p += 4;
      const char *pname = (const char *)(strs + noff);
      const uint8_t *val = p;
      if (str_eq(pname, "reg") && len >= 16) {
        uint64_t addr = be64(val);
        uint64_t size = be64(val + 8);
        if (str_pfx(cur, "pl011")) {
          g_uart = (volatile uint32_t *)(uintptr_t)addr;
        } else if (str_pfx(cur, "memory")) {
          g_mem_base = addr;
          g_mem_size = size;
        }
      }
      p += len;
      p = (const uint8_t *)(((uintptr_t)p + 3) & ~3UL);
    } else if (tok == FDT_END_NODE || tok == FDT_NOP) {
      /* nothing */
    } else { /* FDT_END or unknown */
      break;
    }
  }
}

void mini_main(uint64_t dtb_ipa) {
  const uint8_t *dtb = (const uint8_t *)(uintptr_t)dtb_ipa;
  const struct fdt_header *h = (const struct fdt_header *)dtb;

  /* If the DTB is bad we have no UART to complain through; just stop. */
  if (be32(h->magic) != 0xd00dfeedU) {
    return;
  }
  parse_dtb(dtb);
  if (!g_uart) {
    return; /* no UART discovered — can't print */
  }

  uart_puts("miniguest: hello from a non-FermiOS guest!\n");
  uart_puts("miniguest: discovered via DTB -> uart=");
  uart_hex((uint64_t)(uintptr_t)g_uart);
  uart_puts(" mem_base=");
  uart_hex(g_mem_base);
  uart_puts(" mem_size=");
  uart_hex(g_mem_size);
  uart_putc('\n');
  uart_puts("miniguest: done.\n");
}
