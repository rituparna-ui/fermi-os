#include "vpci.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "hyp.h"
#include <stdint.h>

/* PCI config-space register offsets (type-0 header). */
#define CFG_VENDOR   0x00 /* u16 */
#define CFG_DEVICE   0x02 /* u16 */
#define CFG_COMMAND  0x04 /* u16 */
#define CFG_STATUS   0x06 /* u16 */
#define CFG_REVISION 0x08 /* u8  */
#define CFG_CLASS    0x09 /* 3 bytes prog-if/subclass/class */
#define CFG_HDR_TYPE 0x0E /* u8  */
#define CFG_BAR0     0x10 /* u32 */
#define CFG_BAR1     0x14
#define CFG_SUBVEN   0x2C
#define CFG_SUBDEV   0x2E
#define CFG_CAP_PTR  0x34 /* u8  */
#define CFG_INT_LINE 0x3C /* u8  */
#define CFG_INT_PIN  0x3D /* u8  */

#define VPCI_VENDOR  0x1234
#define VPCI_DEVICE  0xBEEF
#define VPCI_CLASS   0xFF0000u /* class 0xFF (unassigned), subclass/prog-if 0 */

/* BAR0: a 64 KiB 32-bit non-prefetchable memory BAR. Memory BAR low bits:
 * bit0=0 (memory), bits[2:1]=00 (32-bit), bit3=0 (non-prefetchable). */
#define BAR0_SIZE   0x10000u
#define BAR0_TYPE   0x0u
#define BAR0_SIZEMASK (~(BAR0_SIZE - 1) | BAR0_TYPE) /* readback after FFFFFFFF */

/* --- MSI-X capability (PCI cap ID 0x11) at config offset 0x40 ------------- */
#define CFG_MSIX_CAP     0x40 /* 4-aligned dword: [15:0]=ID|next, [31:16]=Msg Control */
/* NOTE: Message Control is byte 0x42 = the HIGH 16 bits of the 0x40 dword. There
 * is NO separate switch case for 0x42 — `reg = off & ~3` is always 0x40. Adding
 * `case 0x42` would never match and would silently drop MSI-X Enable writes. */
#define CFG_MSIX_TBL     0x44 /* u32 Table Offset|BIR (read-only) */
#define CFG_MSIX_PBA     0x48 /* u32 PBA   Offset|BIR (read-only) */

#define MSIX_CAP_ID      0x11
#define MSIX_N           VPCI_MSIX_NVEC /* table size (vectors) */
#define MSIX_BIR         1             /* table + PBA live in BAR1 */
#define MSIX_TBL_OFF     0x0000        /* table at BAR1 + 0x000 */
#define MSIX_PBA_OFF     0x0800        /* PBA   at BAR1 + 0x800 */
#define MSIX_DOORBELL_OFF 0x0C00       /* WO doorbell (emulated device signal) */

/* Message Control: bits[10:0]=TableSize-1; bit14=Function Mask(RW); bit15=Enable(RW). */
#define MSIX_CTRL_TSIZE  (MSIX_N - 1)
#define MSIX_CTRL_ENABLE 0x8000
#define MSIX_CTRL_FMASK  0x4000
#define MSIX_CTRL_RW     (MSIX_CTRL_ENABLE | MSIX_CTRL_FMASK)

/* BAR1: 4 KiB 32-bit non-prefetchable memory BAR, FIXED base (non-relocatable):
 * it holds the MSI-X table/PBA, which must stay at the dedicated trapping window
 * 0x0A005000 (a guest-chosen base could not be left stage-2-invalid). */
#define BAR1_SIZE     0x1000u
#define BAR1_TYPE     0x0u
#define BAR1_SIZEMASK (~(BAR1_SIZE - 1) | BAR1_TYPE)

/* Layout coupling guards: table / PBA / doorbell must not overlap. */
_Static_assert(MSIX_N * 16 <= MSIX_PBA_OFF, "MSI-X table overlaps PBA");
_Static_assert(MSIX_PBA_OFF + 4 <= MSIX_DOORBELL_OFF, "MSI-X PBA overlaps doorbell");
_Static_assert(MSIX_DOORBELL_OFF < BAR1_SIZE, "doorbell outside BAR1 window");

/* One MSI-X table entry (16 bytes). */
typedef struct {
  uint32_t addr_lo;  /* Msg Addr Lo  (recorded; never dereferenced) */
  uint32_t addr_hi;  /* Msg Addr Hi  (recorded; never dereferenced) */
  uint32_t data;     /* Msg Data     (low bits select the SPI) */
  uint32_t vec_ctrl; /* bit0 = Mask  (RW); rest RAZ/WI */
} msix_entry_t;

static struct {
  uint16_t command;
  uint32_t bar0;       /* programmed base (type bits in low) */
  int      bar0_sizing;/* set when guest wrote all-ones, next read = size mask */
  /* --- MSI-X --- */
  uint16_t msix_ctrl;  /* Message Control (only RW bits 14/15 honored) */
  int      bar1_sizing;/* BAR1 is fixed-base; only the sizing probe is tracked */
  int      owner_id;   /* vcpu that owns the device; -1 = unbound (id 0 is valid!) */
  int      inited;     /* one-time-init guard */
} vpci;

static msix_entry_t msix_table[MSIX_N]; /* vec_ctrl Mask defaults to 1 (see init) */
static uint32_t     msix_pba;           /* bit V = vector V pending while undeliverable */

static void vpci_msix_flush_pending(void); /* fwd decl (sole PBA consumer) */

/* One-time init: the vpci struct is zero-initialised, but owner_id must start -1,
 * msix_ctrl must read back as the table size, and every vec_ctrl Mask bit must
 * default to 1 BEFORE the guest's first read. Called first in BOTH MMIO entry
 * points so whichever window the guest touches first initialises state. */
static void vpci_msix_init(void) {
  if (vpci.inited) {
    return;
  }
  vpci.inited = 1;
  vpci.owner_id = -1;                  /* unbound sentinel (NOT 0) */
  vpci.msix_ctrl = MSIX_CTRL_TSIZE;    /* table size; disabled; function-unmasked */
  for (uint32_t i = 0; i < MSIX_N; i++) {
    msix_table[i].vec_ctrl = 1;        /* all vectors masked at reset */
  }
  msix_pba = 0;
}

/* Assemble the full 32-bit value of config register `reg` (reg is 4-aligned). */
static uint32_t cfg_read32(uint32_t reg) {
  switch (reg) {
  case CFG_VENDOR:  return VPCI_VENDOR | ((uint32_t)VPCI_DEVICE << 16);
  case CFG_COMMAND: return vpci.command | (0x0010u << 16); /* status: cap list */
  case CFG_REVISION:return (VPCI_CLASS << 8) | 0x01;       /* rev 1 + class */
  case CFG_HDR_TYPE:return 0x0;                             /* type 0, single fn */
  case CFG_BAR0:    return vpci.bar0_sizing ? BAR0_SIZEMASK : vpci.bar0;
  case CFG_BAR1:
    /* FIXED-base memory BAR. Sizing probe returns the size mask; otherwise the
     * pinned base. The guest cannot relocate it (write path ignores base). */
    return vpci.bar1_sizing ? BAR1_SIZEMASK
                            : ((uint32_t)VPCI_MSIX_BASE | BAR1_TYPE);
  case CFG_SUBVEN:  return VPCI_VENDOR | ((uint32_t)VPCI_DEVICE << 16);
  case CFG_CAP_PTR: return CFG_MSIX_CAP; /* capability list -> MSI-X cap */
  case CFG_MSIX_CAP:return MSIX_CAP_ID | (0x00u << 8)      /* byte1 next-ptr = 0 */
                          | ((uint32_t)vpci.msix_ctrl << 16);
  case CFG_MSIX_TBL:return (MSIX_TBL_OFF & ~0x7u) | MSIX_BIR;
  case CFG_MSIX_PBA:return (MSIX_PBA_OFF & ~0x7u) | MSIX_BIR;
  case CFG_INT_LINE:return 0x00 | (0x01u << 8); /* int pin A, line 0 */
  default:          return 0;
  }
}

int vpci_mmio_is_target(uint64_t ipa) {
  return ipa >= VPCI_ECAM_BASE && ipa < VPCI_ECAM_BASE + VPCI_ECAM_SIZE;
}

void vpci_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                       int size_bytes) {
  vpci_msix_init();
  if (vpci.owner_id < 0 && cur_vcpu) {
    vpci.owner_id = (int)cur_vcpu->id; /* bind to first toucher (-1 = unbound) */
  }

  /* Within the 4 KiB window: only bus0/slot0/func0 (offset = ipa - base, the
   * low 12 bits of the ECAM address = the config register). */
  uint32_t off = (uint32_t)(ipa - VPCI_ECAM_BASE);
  uint32_t reg = off & ~3u;       /* 4-byte aligned register */
  uint32_t byte = off & 3u;       /* sub-word offset */
  uint32_t bitsh = byte * 8;
  uint32_t width_mask = (size_bytes >= 4) ? 0xFFFFFFFFu
                                          : ((1u << (size_bytes * 8)) - 1u);

  if (!is_write) {
    uint32_t full = cfg_read32(reg);
    *val = (full >> bitsh) & width_mask;
    return;
  }

  /* Write: merge the sub-word into the 32-bit register, then act. */
  uint32_t w = (uint32_t)*val & width_mask;
  uint32_t cur = cfg_read32(reg);
  uint32_t merged = (cur & ~(width_mask << bitsh)) | (w << bitsh);

  switch (reg) {
  case CFG_COMMAND:
    vpci.command = (uint16_t)merged; /* guest enables MEM/BUSMASTER here */
    break;
  case CFG_BAR0:
    if (merged == 0xFFFFFFFFu) {
      vpci.bar0_sizing = 1; /* next BAR0 read returns the size mask */
    } else {
      vpci.bar0_sizing = 0;
      vpci.bar0 = (merged & ~(BAR0_SIZE - 1)) | BAR0_TYPE; /* latch base */
    }
    break;
  case CFG_BAR1:
    /* FIXED base: honor only the sizing probe; ignore base writes entirely. */
    vpci.bar1_sizing = (merged == 0xFFFFFFFFu) ? 1 : 0;
    break;
  case CFG_MSIX_CAP: {
    /* Message Control is in merged[31:16]. Only Enable(15) + Function Mask(14)
     * are RW; cap ID, next-ptr, and Table-Size are read-only. */
    uint16_t new_ctrl = (uint16_t)(merged >> 16);
    uint16_t old_ctrl = vpci.msix_ctrl;
    /* INVARIANT (do NOT reorder): update msix_ctrl FIRST, THEN flush, so the
     * flush observes the new Enable/FuncMask state. Reversing these two lines
     * silently breaks Enable-edge and Function-unmask deferred delivery. */
    vpci.msix_ctrl = (old_ctrl & ~MSIX_CTRL_RW) | (new_ctrl & MSIX_CTRL_RW);
    vpci_msix_flush_pending(); /* sole PBA consumer; guarded + idempotent */
    break;
  }
  case CFG_MSIX_TBL:
  case CFG_MSIX_PBA:
    break; /* read-only */
  default:
    break; /* other config regs are read-only here */
  }
}

/* ---------------------------------------------------------------------------
 * MSI-X table / PBA / doorbell window (BAR1 @ VPCI_MSIX_BASE).
 *
 * The table+PBA live in this EL2-local struct (not guest RAM), so no cache
 * maintenance is needed. The guest's reads/writes of the BAR1 window all trap
 * here. Modeled as 32-bit registers; a 64-bit access is split into two 32-bit
 * ops so a single STR of {addr_lo,addr_hi} honors both halves.
 * ------------------------------------------------------------------------- */

/* Map a vector's Msg Data to an SPI, or 0 if out of the device's range. DROPS
 * (returns 0) rather than rewriting an out-of-range value: a guest can never
 * inject INTID < 32 or any foreign SPI, and dropping surfaces driver bugs. */
static uint32_t msix_spi_for(uint32_t vec) {
  uint32_t spi = msix_table[vec].data & 0xFFFFFu; /* low 20 bits = INTID field */
  if (spi < VPCI_MSIX_SPI_BASE || spi >= VPCI_MSIX_SPI_BASE + MSIX_N) {
    return 0;
  }
  return spi;
}

/* Inject vector `vec`'s SPI into the owner. Returns 1 iff actually enqueued into
 * a live List Register (so the PBA bit can be cleared). Owner-guarded: the live
 * LR path is only correct when cur_vcpu IS the owner (always true at trap time —
 * only the owner's stage-2 leaves the window invalid). */
static int msix_deliver(uint32_t vec) {
  if (!cur_vcpu || (int)cur_vcpu->id != vpci.owner_id) {
    return 0;
  }
  uint32_t spi = msix_spi_for(vec);
  if (spi == 0) {
    return 0; /* out-of-range Msg Data: drop */
  }
  return vgic_inject_spi_try(spi); /* 0 if no free LR */
}

/* SOLE consumer of PBA bits. Idempotent. Called on every delivery-enabling edge:
 * MSI-X Enable 0->1, Function Mask 1->0 (both via CFG_MSIX_CAP), and per-vector
 * Mask 1->0 (via the table write). Clears a PBA bit ONLY on confirmed enqueue,
 * so a full-LR drop leaves the interrupt pending (no lost interrupt). */
static void vpci_msix_flush_pending(void) {
  if (!(vpci.msix_ctrl & MSIX_CTRL_ENABLE) || (vpci.msix_ctrl & MSIX_CTRL_FMASK)) {
    return;
  }
  for (uint32_t v = 0; v < MSIX_N; v++) {
    if ((msix_pba & (1u << v)) && !(msix_table[v].vec_ctrl & 1)) {
      if (msix_deliver(v)) {
        msix_pba &= ~(1u << v); /* clear ONLY on confirmed enqueue */
      }
    }
  }
}

/* Guest rang the doorbell asking the device to fire vector `vec`. If MSI-X is
 * disabled, the function is masked, or the vector is masked, latch it in the PBA
 * for deferred delivery; otherwise inject now (latching it if the LRs are full). */
static void vpci_msix_doorbell(uint32_t vec) {
  if (vec >= MSIX_N) {
    return; /* vector index bounds */
  }
  int enabled = (vpci.msix_ctrl & MSIX_CTRL_ENABLE) != 0;
  int fmask = (vpci.msix_ctrl & MSIX_CTRL_FMASK) != 0;
  int vmask = (msix_table[vec].vec_ctrl & 1) != 0;
  if (!enabled || fmask || vmask) {
    msix_pba |= (1u << vec); /* not deliverable now -> pend it */
    return;
  }
  if (!msix_deliver(vec)) {
    msix_pba |= (1u << vec); /* LRs full / dropped -> stay pending */
  }
}

/* 32-bit register read of the BAR1 window at byte offset `reg` (4-aligned). */
static uint32_t msix_reg_read32(uint32_t reg) {
  if (reg < MSIX_N * 16) { /* TABLE region (bounded index) */
    uint32_t idx = reg / 16, field = (reg % 16) / 4;
    switch (field) {
    case 0:  return msix_table[idx].addr_lo;
    case 1:  return msix_table[idx].addr_hi;
    case 2:  return msix_table[idx].data;
    default: return msix_table[idx].vec_ctrl & 1u; /* only Mask bit observable */
    }
  }
  if (reg == MSIX_PBA_OFF) {
    return msix_pba & ((1u << MSIX_N) - 1u); /* PBA, read-only */
  }
  return 0; /* doorbell reads 0; reserved reads 0 */
}

/* 32-bit register write of the BAR1 window. */
static void msix_reg_write32(uint32_t reg, uint32_t v) {
  if (reg < MSIX_N * 16) { /* TABLE region (bounded index) */
    uint32_t idx = reg / 16, field = (reg % 16) / 4;
    switch (field) {
    case 0: msix_table[idx].addr_lo = v; break;
    case 1: msix_table[idx].addr_hi = v; break;
    case 2: msix_table[idx].data = v; break;
    default: {
      /* vec_ctrl: only the Mask bit is RW. */
      uint32_t was_masked = msix_table[idx].vec_ctrl & 1u;
      msix_table[idx].vec_ctrl = v & 1u;
      /* Mask 1->0 (unmask): route to the SINGLE PBA consumer; do NOT inline-
       * deliver here (one writer of PBA bits only -> no double-fire). */
      if (was_masked && !(v & 1u)) {
        vpci_msix_flush_pending();
      }
      break;
    }
    }
    return;
  }
  if (reg == MSIX_PBA_OFF) {
    return; /* PBA is read-only to the guest */
  }
  if (reg == MSIX_DOORBELL_OFF) {
    vpci_msix_doorbell(v);
    return;
  }
  /* reserved: ignore */
}

int vpci_msix_mmio_is_target(uint64_t ipa) {
  return ipa >= VPCI_MSIX_BASE && ipa < VPCI_MSIX_BASE + VPCI_MSIX_SIZE;
}

void vpci_msix_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                            int size_bytes) {
  vpci_msix_init();
  if (vpci.owner_id < 0 && cur_vcpu) {
    vpci.owner_id = (int)cur_vcpu->id;
  }

  uint32_t off = (uint32_t)(ipa - VPCI_MSIX_BASE);

  /* 64-bit access: split into two 32-bit register ops so a single STR of
   * {addr_lo,addr_hi} honors BOTH halves. An 8-byte access is naturally
   * 8-aligned, so `off` and `off+4` are both 4-aligned register offsets. */
  if (size_bytes == 8) {
    if (is_write) {
      msix_reg_write32(off, (uint32_t)(*val));
      msix_reg_write32(off + 4, (uint32_t)(*val >> 32));
    } else {
      uint32_t lo = msix_reg_read32(off);
      uint32_t hi = msix_reg_read32(off + 4);
      *val = ((uint64_t)hi << 32) | lo;
    }
    return;
  }

  uint32_t reg = off & ~3u;
  uint32_t bitsh = (off & 3u) * 8;
  uint32_t wmask = (size_bytes >= 4) ? 0xFFFFFFFFu
                                     : ((1u << (size_bytes * 8)) - 1u);
  uint32_t cur = msix_reg_read32(reg);

  if (!is_write) {
    *val = (cur >> bitsh) & wmask;
    return;
  }

  uint32_t w = (uint32_t)*val & wmask;
  uint32_t merged = (cur & ~(wmask << bitsh)) | (w << bitsh);
  msix_reg_write32(reg, merged);
}
