#include "vpci.h"
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

static struct {
  uint16_t command;
  uint32_t bar0;       /* programmed base (type bits in low) */
  int      bar0_sizing;/* set when guest wrote all-ones, next read = size mask */
} vpci;

/* Assemble the full 32-bit value of config register `reg` (reg is 4-aligned). */
static uint32_t cfg_read32(uint32_t reg) {
  switch (reg) {
  case CFG_VENDOR:  return VPCI_VENDOR | ((uint32_t)VPCI_DEVICE << 16);
  case CFG_COMMAND: return vpci.command | (0x0010u << 16); /* status: cap list */
  case CFG_REVISION:return (VPCI_CLASS << 8) | 0x01;       /* rev 1 + class */
  case CFG_HDR_TYPE:return 0x0;                             /* type 0, single fn */
  case CFG_BAR0:    return vpci.bar0_sizing ? BAR0_SIZEMASK : vpci.bar0;
  case CFG_BAR1:    return 0; /* unused */
  case CFG_SUBVEN:  return VPCI_VENDOR | ((uint32_t)VPCI_DEVICE << 16);
  case CFG_CAP_PTR: return 0; /* no capabilities */
  case CFG_INT_LINE:return 0x00 | (0x01u << 8); /* int pin A, line 0 */
  default:          return 0;
  }
}

int vpci_mmio_is_target(uint64_t ipa) {
  return ipa >= VPCI_ECAM_BASE && ipa < VPCI_ECAM_BASE + VPCI_ECAM_SIZE;
}

void vpci_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                       int size_bytes) {
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
  default:
    break; /* other config regs are read-only here */
  }
}
