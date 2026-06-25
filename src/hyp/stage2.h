#ifndef HYP_STAGE2_H
#define HYP_STAGE2_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Stage-2 translation (guest IPA -> host PA) for the FermiOS hypervisor.
 *
 * Stage-2 descriptors have DIFFERENT semantics from the guest's stage-1 PTEs:
 *   - Access permission is a flat 2-bit S2AP[7:6] (no EL1/EL0 split).
 *     CRITICAL: stage-1 PTE_AP_RW is (0<<6) which at stage-2 means NO-ACCESS;
 *     reusing it would fault every guest access. Use S2_S2AP_RW = (3<<6).
 *   - Memory type is encoded DIRECTLY in MemAttr[5:2] — there is NO MAIR at
 *     stage-2. Normal-WB = 0b1111, Device-nGnRE = 0b0001.
 *   - XN is the 2-bit enumerated field [54:53] (FEAT_XNX, present on -cpu max):
 *       0b00 = executable at EL1 and EL0
 *       0b01 = execute-never at EL0 only
 *       0b10 = execute-never at EL1 and EL0   <- use for device MMIO
 *       0b11 = execute-never at EL1, executable at EL0
 * The lower type bits [1:0], AF, SH, and the output-address mask are shared
 * with stage-1, so we reuse those macros from mm/mmu/mmu.h.
 * ------------------------------------------------------------------------- */

/* Architectural descriptor bits shared with stage-1 (defined here so the hyp
 * image stays independent of the guest's mm/mmu/mmu.h). */
#define PTE_VALID     (1ULL << 0)
#define PTE_TABLE     (1ULL << 1)  /* table (L1/L2) or page (L3) descriptor */
#define PTE_BLOCK     (0ULL << 1)  /* block descriptor (L1=1GiB, L2=2MiB)   */
#define PTE_AF        (1ULL << 10) /* access flag — must be 1               */
#define PTE_SH_INNER  (3ULL << 8)  /* inner shareable                       */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/* Block / page sizes. */
#define S2_1GB 0x40000000ULL
#define S2_2MB 0x00200000ULL
#define S2_PAGE 0x00001000ULL

/* Stage-2 walk indices. The start level (L1) is a CONCATENATED 1024-entry
 * table for the 40-bit IPA / SL0=1 layout, so its index is 10 bits, NOT the
 * 9-bit stage-1 L1 index. L2/L3 are the standard 9-bit indices. */
#define S2_L1_INDEX(ipa) (((ipa) >> 30) & 0x3FF)
#define S2_L2_INDEX(ipa) (((ipa) >> 21) & 0x1FF)
#define S2_L3_INDEX(ipa) (((ipa) >> 12) & 0x1FF)

/* Stage-2 access permissions (S2AP[7:6]). */
#define S2_S2AP_NONE (0ULL << 6)
#define S2_S2AP_RO   (1ULL << 6)
#define S2_S2AP_RW   (3ULL << 6)

/* Stage-2 memory attributes (MemAttr[5:2], direct — NOT a MAIR index). */
#define S2_MEMATTR_DEVICE_nGnRnE (0b0000ULL << 2)
#define S2_MEMATTR_DEVICE_nGnRE  (0b0001ULL << 2)
#define S2_MEMATTR_NORMAL_NC     (0b0101ULL << 2)
#define S2_MEMATTR_NORMAL_WB     (0b1111ULL << 2)

/* Stage-2 execute-never (XN[54:53], FEAT_XNX 2-bit enumerated field). */
#define S2_XN_NONE     (0b00ULL << 53) /* executable at EL1 and EL0 */
#define S2_XN_ALL      (0b10ULL << 53) /* execute-never at EL1 and EL0 */

/* Single guest => fixed VMID 1 (VMID 0 reserved by convention). */
#define HYP_VMID 1ULL

/* Program VTCR_EL2 (one-time, shared by all VMs — same IPA geometry). */
void s2_init_vtcr(void);

/* Build VM1's (FermiOS) stage-2 identity map (IPA==PA) and return its L1 root
 * host PA. Maps 8 GiB RAM + device windows; GICD/GICR left invalid to trap. */
uint64_t s2_build_vm1(void);

/* Build VM2's stage-2: guest RAM IPA [0x40000000, 0x40000000+ram_size) -> host
 * PA [host_ram_base, ...), plus the UART (Device, straight-through IPA==PA).
 * Demonstrates isolation — VM2's IPA 0x40000000 maps to host_ram_base, not
 * 0x40000000. Returns the L1 root host PA. */
uint64_t s2_build_vm2(uint64_t host_ram_base, uint64_t ram_size);

/* Map [ipa, ipa+size) -> [pa, pa+size) into the stage-2 table rooted at l1.
 *   device != 0 : Device-nGnRE + execute-never; else Normal-WB + executable. */
void s2_map_range_in(uint64_t *l1, uint64_t ipa, uint64_t pa, uint64_t size,
                     int device);

/* Compose VTTBR_EL2 from an L1 root + VMID. */
uint64_t s2_make_vttbr(uint64_t l1_root, uint32_t vmid);

/* Full stage-2 + stage-1 TLB flush for the current VMID (inner-shareable).
 * Call after building or editing the tables (VTTBR_EL2.VMID must be set). */
void s2_tlb_flush_all(void);

#endif /* HYP_STAGE2_H */
