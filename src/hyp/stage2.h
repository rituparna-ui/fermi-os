#ifndef HYP_STAGE2_H
#define HYP_STAGE2_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Stage-2 translation (guest IPA -> host PA) for the FermiOS VHE hypervisor.
 *
 * Unlike the standalone non-VHE reference, OUR host runs with its stage-1 MMU
 * ON (higher-half kernel). So table pages are allocated from the PMM (physical)
 * and written through PHYS_TO_VIRT (the TTBR1 high-half identity map), while
 * VTTBR_EL2 is programmed with the PHYSICAL L1 root. Because the host memory is
 * Normal-WB Inner-Shareable and VTCR_EL2.{IRGN,ORGN}=WBWA make the stage-2 walk
 * cacheable+IS, a `dsb ish` after the descriptor stores is sufficient for the
 * walker to observe them (no dc cvac needed).
 *
 * Stage-2 descriptors differ from the guest's stage-1 PTEs:
 *   - Access permission is a flat S2AP[7:6]; S2_S2AP_RW = (3<<6).
 *     (stage-1 AP_RW is (0<<6), which at stage-2 means NO-ACCESS — do not reuse)
 *   - Memory type is encoded directly in MemAttr[5:2]; there is NO MAIR.
 *   - XN is the 2-bit field [54:53] (FEAT_XNX, present on -cpu max).
 * ------------------------------------------------------------------------- */

/* Architectural descriptor bits (kept local so the hyp stays self-contained). */
#define S2_PTE_VALID  (1ULL << 0)
#define S2_PTE_TABLE  (1ULL << 1)  /* table (L1/L2) or page (L3) descriptor */
#define S2_PTE_BLOCK  (0ULL << 1)  /* block descriptor (L1=1GiB, L2=2MiB)   */
#define S2_PTE_AF     (1ULL << 10) /* access flag — must be 1               */
#define S2_PTE_SH_IS  (3ULL << 8)  /* inner shareable                       */
#define S2_ADDR_MASK  0x0000FFFFFFFFF000ULL

#define S2_1GB  0x40000000ULL
#define S2_2MB  0x00200000ULL
#define S2_PAGE 0x00001000ULL

/* Walk indices. Start level (L1) is a CONCATENATED 1024-entry table for the
 * 40-bit IPA / SL0=1 layout -> 10-bit index. L2/L3 are standard 9-bit. */
#define S2_L1_INDEX(ipa) (((ipa) >> 30) & 0x3FF)
#define S2_L2_INDEX(ipa) (((ipa) >> 21) & 0x1FF)
#define S2_L3_INDEX(ipa) (((ipa) >> 12) & 0x1FF)

/* S2AP[7:6]. */
#define S2_S2AP_NONE (0ULL << 6)
#define S2_S2AP_RO   (1ULL << 6)
#define S2_S2AP_RW   (3ULL << 6)

/* MemAttr[5:2] (direct, not a MAIR index). */
#define S2_MEMATTR_DEVICE_nGnRE (0b0001ULL << 2)
#define S2_MEMATTR_NORMAL_WB    (0b1111ULL << 2)

/* XN[54:53] (FEAT_XNX 2-bit enumerated field). */
#define S2_XN_NONE (0b00ULL << 53) /* executable at EL1 and EL0      */
#define S2_XN_ALL  (0b10ULL << 53) /* execute-never at EL1 and EL0   */

/* VTCR_EL2: T0SZ=24 (40-bit IPA), SL0=1 (start L1), IRGN/ORGN=WBWA, SH=IS,
 * TG0=4K, PS=40b, VS=8-bit VMID, RES1 bit31. */
#define S2_VTCR_EL2 0x80023558ULL

/* An opaque stage-2 address space. Built with pmm pages; root_phys is what
 * goes into VTTBR_EL2 (with the VMID in [55:48]). */
typedef struct stage2 {
  uint64_t *l1_virt;   /* PHYS_TO_VIRT(root_phys) — used to edit tables */
  uint64_t  root_phys; /* physical base for VTTBR_EL2 (8 KiB aligned)   */
  uint32_t  vmid;
} stage2_t;

/* Initialise an empty stage-2 (allocates the concatenated L1). */
int stage2_create(stage2_t *s2, uint32_t vmid);

/* Map [ipa,ipa+size) -> [pa,pa+size). device!=0 => Device-nGnRE + XN; else
 * Normal-WB + executable. ipa/pa/size must be 4 KiB aligned. */
void stage2_map(stage2_t *s2, uint64_t ipa, uint64_t pa, uint64_t size, int device);

/* VTTBR_EL2 value (root_phys | VMID<<48) for this address space. */
uint64_t stage2_vttbr(const stage2_t *s2);

/* Translate a guest IPA to its host physical address by walking THIS guest's
 * stage-2 tables. Returns the host PA on success, or 0 if the IPA is not mapped
 * in this guest (the isolation check). This is the safe primitive for the
 * hypervisor to touch a guest-supplied buffer: a guest can only ever name
 * memory the hypervisor already granted it. */
uint64_t stage2_translate(const stage2_t *s2, uint64_t ipa);

#endif /* HYP_STAGE2_H */
