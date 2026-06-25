#include "hyp.h"
#include "hyp_alloc.h"
#include "hyp_gic.h"
#include "hyp_sysregs.h"
#include "stage2.h"
#include "timer/vtimer.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EL2 hypervisor C core (Milestone 1).
 *
 * Everything here runs at EL2 with the MMU OFF, so all addresses are physical.
 * We cannot reuse the guest's uart.c: it is part of the guest image and its
 * symbols live at guest VAs. The PL011 UART is at the same physical address
 * (0x09000000) for both, so a tiny self-contained poke is all we need. QEMU
 * has already initialised the PL011 enough for output at reset, and the guest
 * re-initialises it anyway, so we only touch the data register.
 * ------------------------------------------------------------------------- */

#define HYP_UART_BASE 0x09000000UL
#define HYP_UART_DR   (HYP_UART_BASE + 0x00)
#define HYP_UART_FR   (HYP_UART_BASE + 0x18)
#define HYP_UART_FR_TXFF (1U << 5) /* transmit FIFO full */

static inline void mmio_w32(uint64_t addr, uint32_t val) {
  *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_r32(uint64_t addr) {
  return *(volatile uint32_t *)addr;
}

void hyp_putc(char c) {
  /* Spin while the TX FIFO is full, then push the byte. */
  while (mmio_r32(HYP_UART_FR) & HYP_UART_FR_TXFF) {
  }
  mmio_w32(HYP_UART_DR, (uint32_t)(unsigned char)c);
}

void hyp_puts(const char *s) {
  for (; *s; s++) {
    if (*s == '\n') {
      hyp_putc('\r');
    }
    hyp_putc(*s);
  }
}

void hyp_puthex(uint64_t v) {
  static const char digits[] = "0123456789ABCDEF";
  hyp_putc('0');
  hyp_putc('x');
  for (int shift = 60; shift >= 0; shift -= 4) {
    hyp_putc(digits[(v >> shift) & 0xF]);
  }
}

/* Called from the EL2 vector stubs (hyp_vectors.S). At Milestone 1 no vector
 * should ever fire, so reaching here means something unexpected trapped to
 * EL2. Report the index + syndrome and halt. Does not return. */
__attribute__((noreturn)) void hyp_vector_report(uint64_t index, uint64_t esr,
                                                 uint64_t elr, uint64_t far) {
  hyp_puts("\n[HYP][TRAP] unexpected EL2 exception, vector index = ");
  hyp_puthex(index);
  hyp_puts("\n  ESR_EL2 = ");
  hyp_puthex(esr);
  hyp_puts("\n  ELR_EL2 = ");
  hyp_puthex(elr);
  hyp_puts("\n  FAR_EL2 = ");
  hyp_puthex(far);
  hyp_putc('\n');
  hyp_panic("unexpected EL2 trap (M1: nothing should trap to EL2)");
}

__attribute__((noreturn)) void hyp_panic(const char *msg) {
  hyp_puts("\n[HYP][PANIC] ");
  if (msg) {
    hyp_puts(msg);
  }
  hyp_putc('\n');
  for (;;) {
    __asm__ __volatile__("wfe");
  }
}

/* Guest flat image embedded in the hyp (see guest_blob.S). */
extern const uint8_t __guest_blob_start[];
extern const uint8_t __guest_blob_end[];

/* Copy the embedded guest image to its physical load base (0x40000000) so the
 * eret lands on real guest bytes. Done with the EL2 MMU off (physical stores).
 * The guest's first LOAD segment starts at PA 0x40000000 and the flat blob
 * preserves inter-segment gaps, so one linear copy places every PT_LOAD. */
static void hyp_load_guest(void) {
  const uint8_t *src = __guest_blob_start;
  uint64_t size = (uint64_t)(__guest_blob_end - __guest_blob_start);
  volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)GUEST_ENTRY_IPA;

  /* Word copy for the aligned bulk, byte copy for the tail. */
  uint64_t words = size / 8;
  const uint64_t *s64 = (const uint64_t *)src;
  volatile uint64_t *d64 = (volatile uint64_t *)dst;
  for (uint64_t i = 0; i < words; i++) {
    d64[i] = s64[i];
  }
  for (uint64_t i = words * 8; i < size; i++) {
    dst[i] = src[i];
  }
  /* Ensure the stores are visible and instruction-coherent before the guest
   * (at EL1) fetches them: clean D-cache then invalidate I-cache to PoU. */
  hyp_dcache_clean_range(GUEST_ENTRY_IPA, size);
  __asm__ __volatile__("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

  hyp_puts("[HYP] guest image copied to ");
  hyp_puthex(GUEST_ENTRY_IPA);
  hyp_puts(" (");
  hyp_puthex(size);
  hyp_puts(" bytes)\n");
}

/* VM2's flat image, embedded in the hyp (guest2_blob.S). */
extern const uint8_t __guest2_blob_start[];
extern const uint8_t __guest2_blob_end[];

/* VM2's RAM lives above the hypervisor image, in the top reserved GiB. The hyp
 * is at 0x250000000; place VM2's RAM at 0x260000000 (well clear of the hyp's
 * own pool which grows up from ~0x250100000). 64 MiB is ample for the tiny
 * guest and keeps the stage-2 map to whole 2 MiB blocks. */
#define VM2_HOST_RAM_BASE 0x260000000ULL
#define VM2_RAM_SIZE      0x04000000ULL /* 64 MiB */

/* Copy a flat blob to a host physical destination (EL2 MMU off). */
static void copy_blob(uint64_t dst_pa, const uint8_t *src, uint64_t size) {
  volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst_pa;
  const uint64_t *s = (const uint64_t *)src;
  uint64_t words = size / 8;
  for (uint64_t i = 0; i < words; i++) d[i] = s[i];
  volatile uint8_t *db = (volatile uint8_t *)(uintptr_t)dst_pa;
  for (uint64_t i = words * 8; i < size; i++) db[i] = src[i];
  hyp_dcache_clean_range(dst_pa, size);
  __asm__ __volatile__("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

/* Called from hyp_boot.S after the EL2 register context is established. Sets up
 * stage-2 for both VMs, the GIC/timer virtualization, both vCPUs, the EL2
 * scheduler, and enters VM1. Does NOT return (vcpu_run_first erets). */
void hyp_main(void) {
  uint64_t current_el, cptr;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(current_el));
  __asm__ __volatile__("mrs %0, cptr_el2" : "=r"(cptr));

  hyp_puts("\n==================================================\n");
  hyp_puts("  Fermi Hypervisor (EL2) - multi-VM\n");
  hyp_puts("==================================================\n");
  hyp_puts("[HYP] CurrentEL = ");
  hyp_puthex(current_el);
  hyp_puts("  CPTR_EL2 = ");
  hyp_puthex(cptr);
  hyp_putc('\n');

  /* Place both guest images at their host PAs.
   *   VM1 (FermiOS) -> host PA 0x40000000 (== its IPA 0x40000000)
   *   VM2 (tiny)    -> host PA VM2_HOST_RAM_BASE (its IPA 0x40000000 maps here) */
  hyp_load_guest(); /* VM1 -> 0x40000000 */
  copy_blob(VM2_HOST_RAM_BASE, __guest2_blob_start,
            (uint64_t)(__guest2_blob_end - __guest2_blob_start));
  hyp_puts("[HYP] VM2 image copied to ");
  hyp_puthex(VM2_HOST_RAM_BASE);
  hyp_putc('\n');

  /* Stage-2: one VTCR (shared geometry), two L1 roots (isolated spaces). */
  s2_init_vtcr();
  uint64_t vm1_l1 = s2_build_vm1();
  uint64_t vm2_l1 = s2_build_vm2(VM2_HOST_RAM_BASE, VM2_RAM_SIZE);

  /* GIC + timer virtualization. */
  hyp_gic_init();
  vgic_init();
  vtimer_init();

  /* Route physical IRQ/FIQ/SError to EL2, trap WFI/SMC, stage-2 on. */
  uint64_t hcr3 = HCR_EL2_M3, hcr;
  __asm__ __volatile__("msr hcr_el2, %0\n\tisb" ::"r"(hcr3));
  /* HCR_EL2.VM 0->1 changes the translation regime: flush any stale
   * stage-1-only TLB entries (combined stage-1+2 for this VMID) before the
   * first guest entry. The TLB is empty at QEMU boot, but this is required by
   * the architecture and correct on real hardware. */
  __asm__ __volatile__("dsb ish\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb" ::: "memory");
  __asm__ __volatile__("mrs %0, hcr_el2" : "=r"(hcr));
  hyp_puts("[HYP] HCR_EL2 = ");
  hyp_puthex(hcr);
  hyp_puts(" (VM=1, IRQ->EL2, TWI/TSC)\n");

  /* Create the two vCPUs. Both enter at IPA 0x40000000 with their own stage-2.
   * VM1 uses FermiOS's own boot.S stack math (SP set by guest); VM2 sets its
   * own SP in _g2_start, so sp_el1_override is 0 for both. */
  vcpu_alloc("FermiOS", GUEST_ENTRY_IPA, s2_make_vttbr(vm1_l1, 1), 0);
  vcpu_alloc("guest2",  GUEST_ENTRY_IPA, s2_make_vttbr(vm2_l1, 2), 0);
  hyp_puts("[HYP] 2 vCPUs created. Starting EL2 scheduler.\n");
  hyp_puts("--------------------------------------------------\n\n");

  vcpu_sched_init();  /* arm CNTHV scheduler tick */
  vcpu_run_first();   /* enter VM1 — does not return */
}
