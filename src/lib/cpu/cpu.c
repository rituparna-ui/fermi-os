#include "cpu.h"
#include "strings/strings.h"
#include "uart/uart.h"

/* ---------------------------------------------------------------------------
 * cpu.c \u2014 CPU identification + PMU cycle counter
 *
 * Sysregs read here are all standard ARMv8-A:
 *   MIDR_EL1                 : main ID register (implementer / part number)
 *   CTR_EL0                  : cache type (line sizes)
 *   ID_AA64PFR0_EL1          : processor feature register 0 (FP, AdvSIMD)
 *   ID_AA64ISAR0_EL1         : ISA feature register 0 (AES, SHA, CRC32, RNG)
 *   ID_AA64MMFR0_EL1         : memory model register 0 (PARange, TGran)
 *   PMCR_EL0 / PMCCNTR_EL0   : performance monitor cycle counter
 *
 * The PMU enable sequence at boot:
 *   1. Write PMCR_EL0.E = 1 to enable the cycle counter
 *   2. Set PMCNTENSET_EL0[31] = 1 to enable the dedicated cycle counter
 *   3. Optionally PMUSERENR_EL0.EN = 1 to allow EL0 reads (we don't yet)
 * --------------------------------------------------------------------------- */

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

/* Cached at boot so render_info doesn't re-issue MRS on every /proc read.
 * MIDR / CTR / feature registers don't change at runtime. */
static uint64_t g_midr;
static uint64_t g_ctr;
static uint64_t g_pfr0;
static uint64_t g_isar0;
static uint64_t g_mmfr0;

static const char *implementer_name(uint8_t imp) {
  /* Common implementer codes from MIDR_EL1[31:24]. Not exhaustive. */
  switch (imp) {
  case 0x41: return "ARM Limited";
  case 0x42: return "Broadcom";
  case 0x43: return "Cavium";
  case 0x46: return "Fujitsu";
  case 0x48: return "HiSilicon";
  case 0x49: return "Infineon";
  case 0x4D: return "Motorola/Freescale";
  case 0x4E: return "NVIDIA";
  case 0x50: return "Applied Micro";
  case 0x51: return "Qualcomm";
  case 0x53: return "Samsung";
  case 0x54: return "Texas Instruments";
  case 0x56: return "Marvell";
  case 0x61: return "Apple";
  case 0x66: return "Faraday";
  case 0x69: return "Intel";
  case 0xC0: return "Ampere";
  default:   return "Unknown";
  }
}

/* Best-effort part-number lookup for ARM Ltd cores. Used only when
 * Implementer = 0x41. */
static const char *arm_part_name(uint16_t part) {
  switch (part) {
  case 0xD03: return "Cortex-A53";
  case 0xD05: return "Cortex-A55";
  case 0xD07: return "Cortex-A57";
  case 0xD08: return "Cortex-A72";
  case 0xD09: return "Cortex-A73";
  case 0xD0A: return "Cortex-A75";
  case 0xD0B: return "Cortex-A76";
  case 0xD0D: return "Cortex-A77";
  case 0xD40: return "Neoverse-V1";
  case 0xD41: return "Cortex-A78";
  case 0xD49: return "Neoverse-N2";
  case 0xD4A: return "Neoverse-E1";
  default:    return "unknown";
  }
}

void cpu_init(void) {
  /* Snapshot identification registers once. */
  g_midr  = MRS(midr_el1);
  g_ctr   = MRS(ctr_el0);
  g_pfr0  = MRS(id_aa64pfr0_el1);
  g_isar0 = MRS(id_aa64isar0_el1);
  g_mmfr0 = MRS(id_aa64mmfr0_el1);

  /* Reset and enable the cycle counter.
   *   PMCR_EL0[6]  = LC  : long-counter mode (64-bit cycles, no overflow at 2^32)
   *   PMCR_EL0[2]  = C   : reset cycle counter to 0
   *   PMCR_EL0[1]  = P   : reset event counters (we don't use them but harmless)
   *   PMCR_EL0[0]  = E   : enable */
  MSR(pmcr_el0, (1ULL << 6) | (1ULL << 2) | (1ULL << 1) | (1ULL << 0));

  /* PMCNTENSET_EL0[31] = enable the dedicated cycle counter. */
  MSR(pmcntenset_el0, (1ULL << 31));

  uart_printf("[CPU] %s %s r%dp%d, midr=%x\n",
              implementer_name((uint8_t)((g_midr >> 24) & 0xFF)),
              ((g_midr >> 24) & 0xFF) == 0x41
                  ? arm_part_name((uint16_t)((g_midr >> 4) & 0xFFF))
                  : "unknown-part",
              (uint64_t)((g_midr >> 20) & 0xF),
              (uint64_t)(g_midr & 0xF),
              g_midr);
  uart_printf("[CPU] PMU enabled (PMCCNTR_EL0 live)\n");
}

uint64_t cpu_read_cycles(void) {
  return MRS(pmccntr_el0);
}

/* PARange field decoder (ID_AA64MMFR0_EL1[3:0]) \u2192 physical address bits. */
static uint64_t parange_bits(uint64_t mmfr0) {
  switch (mmfr0 & 0xF) {
  case 0: return 32;
  case 1: return 36;
  case 2: return 40;
  case 3: return 42;
  case 4: return 44;
  case 5: return 48;
  case 6: return 52;
  default: return 0;
  }
}

int cpu_render_info(char *buf, size_t len) {
  uint8_t  implementer = (uint8_t)((g_midr >> 24) & 0xFF);
  uint8_t  variant     = (uint8_t)((g_midr >> 20) & 0xF);
  uint8_t  arch        = (uint8_t)((g_midr >> 16) & 0xF);
  uint16_t partnum     = (uint16_t)((g_midr >> 4) & 0xFFF);
  uint8_t  revision    = (uint8_t)(g_midr & 0xF);

  /* CTR_EL0 \u2014 cache line sizes (encoded as log2(words/line)). */
  uint64_t i_words = 1ULL << ((g_ctr >>  0) & 0xF);
  uint64_t d_words = 1ULL << ((g_ctr >> 16) & 0xF);

  /* Feature flags. The "FP" / "AdvSIMD" fields in PFR0 are 4 bits each:
   *   0x0 = present, 0xF = not present, 0x1 = present + half-precision. */
  uint64_t fp_field   = (g_pfr0 >> 16) & 0xF;
  uint64_t simd_field = (g_pfr0 >> 20) & 0xF;
  /* ISAR0 fields (each 4 bits): AES@4, SHA1@8, SHA2@12, CRC32@16, RNG@60. */
  int has_aes   = ((g_isar0 >>  4) & 0xF) != 0;
  int has_sha1  = ((g_isar0 >>  8) & 0xF) != 0;
  int has_sha2  = ((g_isar0 >> 12) & 0xF) != 0;
  int has_crc32 = ((g_isar0 >> 16) & 0xF) != 0;
  int has_rndr  = ((g_isar0 >> 60) & 0xF) != 0;

  uint64_t cycles = cpu_read_cycles();

  return ksnprintf(buf, len,
                   "implementer  : %s (0x%x)\n"
                   "part         : %s (0x%x)\n"
                   "architecture : ARMv8 (0x%x)\n"
                   "variant      : 0x%x\n"
                   "revision     : 0x%x\n"
                   "midr_el1     : %x\n"
                   "icache_line  : %u bytes\n"
                   "dcache_line  : %u bytes\n"
                   "phys_addr    : %u bits\n"
                   "fp           : %s\n"
                   "advsimd      : %s\n"
                   "aes          : %s\n"
                   "sha1         : %s\n"
                   "sha2         : %s\n"
                   "crc32        : %s\n"
                   "rndr         : %s\n"
                   "cycles       : %u\n",
                   implementer_name(implementer), (uint64_t)implementer,
                   implementer == 0x41 ? arm_part_name(partnum) : "unknown",
                   (uint64_t)partnum,
                   (uint64_t)arch,
                   (uint64_t)variant,
                   (uint64_t)revision,
                   g_midr,
                   (uint64_t)(i_words * 4),
                   (uint64_t)(d_words * 4),
                   parange_bits(g_mmfr0),
                   fp_field   != 0xF ? "yes" : "no",
                   simd_field != 0xF ? "yes" : "no",
                   has_aes   ? "yes" : "no",
                   has_sha1  ? "yes" : "no",
                   has_sha2  ? "yes" : "no",
                   has_crc32 ? "yes" : "no",
                   has_rndr  ? "yes" : "no",
                   cycles);
}
