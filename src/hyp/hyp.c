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

/* Guest flat images embedded in the hyp (see guest_blob.S / guest2_blob.S). */
extern const uint8_t __guest_blob_start[];
extern const uint8_t __guest_blob_end[];

/* VM2 + IPC + dom0 guest flat images, embedded in the hyp. */
extern const uint8_t __guest2_blob_start[];
extern const uint8_t __guest2_blob_end[];
extern const uint8_t __ipc_blob_start[];
extern const uint8_t __ipc_blob_end[];
extern const uint8_t __dom0_blob_start[];
extern const uint8_t __dom0_blob_end[];

/* Guest RAM regions in the top reserved GiB (hyp is at 0x250000000, its pool
 * grows up from ~0x250100000). Each region is 2 MiB-aligned and well clear of
 * the others and the hyp pool. */
#define VM2_HOST_RAM_BASE 0x260000000ULL /* VM2 (heartbeat)     64 MiB */
#define VM2_RAM_SIZE      0x04000000ULL
#define IPCP_HOST_RAM_BASE 0x264000000ULL /* IPC producer       16 MiB */
#define IPCC_HOST_RAM_BASE 0x265000000ULL /* IPC consumer       16 MiB */
#define IPC_RAM_SIZE       0x01000000ULL
#define IPC_SHARED_PA      0x266000000ULL /* shared page (both IPC VMs)  */
#define DOM0_HOST_RAM_BASE 0x268000000ULL /* dom0 control domain  64 MiB */
#define DOM0_RAM_SIZE      0x04000000ULL

/* Copy a flat blob to a host physical destination (EL2 MMU off) and make it
 * coherent for guest instruction fetch. Used at boot and on warm reset. */
void hyp_copy_image(uint64_t dst_pa, const uint8_t *src, uint64_t size) {
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

  /* Place each guest image at its host PA. The IPC producer + consumer run the
   * SAME image (role chosen by x0) but at separate private host RAM regions. */
  uint64_t vm1_size = (uint64_t)(__guest_blob_end - __guest_blob_start);
  uint64_t vm2_size = (uint64_t)(__guest2_blob_end - __guest2_blob_start);
  uint64_t ipc_size = (uint64_t)(__ipc_blob_end - __ipc_blob_start);
  uint64_t dom0_size = (uint64_t)(__dom0_blob_end - __dom0_blob_start);
  hyp_copy_image(GUEST_ENTRY_IPA, __guest_blob_start, vm1_size);
  hyp_copy_image(VM2_HOST_RAM_BASE, __guest2_blob_start, vm2_size);
  hyp_copy_image(IPCP_HOST_RAM_BASE, __ipc_blob_start, ipc_size);
  hyp_copy_image(IPCC_HOST_RAM_BASE, __ipc_blob_start, ipc_size);
  hyp_copy_image(DOM0_HOST_RAM_BASE, __dom0_blob_start, dom0_size);
  /* Zero the shared IPC page (its seqno starts at 0). */
  for (volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)IPC_SHARED_PA;
       p < (volatile uint64_t *)(uintptr_t)(IPC_SHARED_PA + 0x1000); p++) {
    *p = 0;
  }
  hyp_dcache_clean_range(IPC_SHARED_PA, 0x1000);
  hyp_puts("[HYP] 4 guest images placed; IPC shared page @ ");
  hyp_puthex(IPC_SHARED_PA);
  hyp_putc('\n');

  /* Stage-2: one VTCR (shared geometry), one L1 root per VM (isolated spaces).
   * The two IPC VMs additionally share one page at IPA 0x50000000. */
  s2_init_vtcr();
  uint64_t vm1_l1 = s2_build_vm1();
  uint64_t vm2_l1 = s2_build_vm2(VM2_HOST_RAM_BASE, VM2_RAM_SIZE);
  uint64_t ipcp_l1 = s2_build_ipc(IPCP_HOST_RAM_BASE, IPC_RAM_SIZE, IPC_SHARED_PA);
  uint64_t ipcc_l1 = s2_build_ipc(IPCC_HOST_RAM_BASE, IPC_RAM_SIZE, IPC_SHARED_PA);
  uint64_t dom0_l1 = s2_build_vm2(DOM0_HOST_RAM_BASE, DOM0_RAM_SIZE); /* private RAM + UART */

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
   * own SP in _g2_start, so sp_el1_override is 0 for both. The image triples
   * let the hypervisor warm-reset a guest on PSCI SYSTEM_RESET. */
  vcpu_t *vm1 = vcpu_alloc("FermiOS", GUEST_ENTRY_IPA, s2_make_vttbr(vm1_l1, 1), 0,
             __guest_blob_start, GUEST_ENTRY_IPA, vm1_size);
  vm1->ram_size = 0x200000000ULL; /* 8 GiB */
  vcpu_t *vm2 = vcpu_alloc("guest2", GUEST_ENTRY_IPA, s2_make_vttbr(vm2_l1, 2), 0,
             __guest2_blob_start, VM2_HOST_RAM_BASE, vm2_size);
  vm2->ram_size = VM2_RAM_SIZE;

  /* The two IPC VMs run the same image; x0 selects the role (2=producer,
   * 3=consumer). Set x0_init AND the live gp.x[0] (the first boot's GP state
   * was already initialised by vcpu_alloc). */
  vcpu_t *prod = vcpu_alloc("ipc-prod", GUEST_ENTRY_IPA,
                            s2_make_vttbr(ipcp_l1, 3), 0,
                            __ipc_blob_start, IPCP_HOST_RAM_BASE, ipc_size);
  prod->x0_init = 2;
  prod->gp.x[0] = 2;
  prod->ram_size = IPC_RAM_SIZE;
  vcpu_t *cons = vcpu_alloc("ipc-cons", GUEST_ENTRY_IPA,
                            s2_make_vttbr(ipcc_l1, 4), 0,
                            __ipc_blob_start, IPCC_HOST_RAM_BASE, ipc_size);
  cons->x0_init = 3;
  cons->gp.x[0] = 3;
  cons->ram_size = IPC_RAM_SIZE;

  /* Wire the doorbell: producer notifies consumer (and vice-versa, so an
   * event-driven consumer can ACK back if it wants). */
  prod->doorbell_target = (int)cons->id;
  cons->doorbell_target = (int)prod->id;

  /* dom0: the privileged control domain. It may issue VMCTL hypercalls to
   * enumerate/pause/resume/reset the other VMs. */
  vcpu_t *dom0 = vcpu_alloc("dom0", GUEST_ENTRY_IPA, s2_make_vttbr(dom0_l1, 5), 0,
                            __dom0_blob_start, DOM0_HOST_RAM_BASE, dom0_size);
  dom0->privileged = 1;
  dom0->ram_size = DOM0_RAM_SIZE;

  hyp_puts("[HYP] 5 vCPUs created (incl. privileged dom0). Starting scheduler.\n");
  hyp_puts("--------------------------------------------------\n\n");

  vcpu_sched_init();  /* arm CNTHV scheduler tick */
  vcpu_run_first();   /* enter VM1 — does not return */
}
