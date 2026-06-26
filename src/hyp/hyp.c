#include "hyp.h"
#include "hyp_alloc.h"
#include "hyp_mmu_el2.h"
#include "hyp_gic.h"
#include "hyp_sysregs.h"
#include "snapshot.h"
#include "virtio/virtio_blk.h"
#include "virtio/virtio_balloon.h"
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
extern const uint8_t __vmtgt_blob_start[];
extern const uint8_t __vmtgt_blob_end[];
extern const uint8_t __crasher_blob_start[];
extern const uint8_t __crasher_blob_end[];
extern const uint8_t __hang_blob_start[];
extern const uint8_t __hang_blob_end[];
extern const uint8_t __rng_blob_start[];
extern const uint8_t __rng_blob_end[];
extern const uint8_t __blk_blob_start[];
extern const uint8_t __blk_blob_end[];
extern const uint8_t __net_blob_start[];
extern const uint8_t __net_blob_end[];
extern const uint8_t __pci_blob_start[];
extern const uint8_t __pci_blob_end[];
extern const uint8_t __smp_blob_start[];
extern const uint8_t __smp_blob_end[];
extern const uint8_t __balloon_blob_start[];
extern const uint8_t __balloon_blob_end[];

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
#define VMTGT_HOST_RAM_BASE 0x26C000000ULL /* migration target     64 MiB */
#define VMTGT_RAM_SIZE      0x04000000ULL
#define CRASH_HOST_RAM_BASE 0x270000000ULL /* fault-isolation crasher 16 MiB */
#define CRASH_RAM_SIZE      0x01000000ULL
#define HANG_HOST_RAM_BASE  0x271000000ULL /* watchdog hangguest   16 MiB */
#define HANG_RAM_SIZE       0x01000000ULL
#define RNG_HOST_RAM_BASE   0x272000000ULL /* virtio-mmio rngclient 16 MiB */
#define RNG_RAM_SIZE        0x01000000ULL
#define BLK_HOST_RAM_BASE   0x273000000ULL /* virtio-mmio blkclient 16 MiB */
#define BLK_RAM_SIZE        0x01000000ULL
#define NET_HOST_RAM_BASE   0x274000000ULL /* virtio-mmio netclient 16 MiB */
#define NET_RAM_SIZE        0x01000000ULL
#define PCI_HOST_RAM_BASE   0x275000000ULL /* vPCI pciclient        16 MiB */
#define PCI_RAM_SIZE        0x01000000ULL
#define SMP_HOST_RAM_BASE   0x276000000ULL /* SMP guest (2 vCPUs)   16 MiB */
#define SMP_RAM_SIZE        0x01000000ULL
#define BALLOON_HOST_RAM_BASE 0x277000000ULL /* virtio-balloon client 16 MiB */
#define BALLOON_RAM_SIZE      0x01000000ULL

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

  /* SMP Phase 0: turn on a minimal EL2 stage-1 MMU so EL2 RAM is Normal-WB
   * Inner-Shareable. This is the foundation for multicore: spinlocks/atomics are
   * only inter-PE correct on cacheable IS memory (NOT on the MMU-off Normal
   * Non-cacheable memory used until now). Build (CPU0 only) then enable. */
  hyp_mmu_el2_build();
  hyp_mmu_el2_enable();
  hyp_puts("[EL2MMU] enabled (SCTLR_EL2.M|C|I); EL2 RAM now cacheable IS\n");

  /* Gate (the single most important Phase-0 check): a CAS to a cacheable-arena
   * word must execute without a Data Abort, proving exclusives/atomics are now
   * usable for the spinlock primitive. With the MMU off this would have been
   * CONSTRAINED UNPREDICTABLE. */
  {
    static volatile uint32_t probe_word; /* lives in .bss -> now cacheable IS */
    uint32_t tmp, fail;
    __asm__ __volatile__(
        "1: ldaxr %w0, [%2]\n\t"   /* load-acquire exclusive */
        "   stlxr %w1, %w3, [%2]\n\t" /* store-release exclusive; %w1=0 on success */
        "   cbnz  %w1, 1b\n\t"
        "   dmb   ish"
        : "=&r"(tmp), "=&r"(fail) : "r"(&probe_word), "r"(0xABCDu) : "memory");
    hyp_puts("[EL2MMU] atomic probe: ldaxr/stlxr ok, word=");
    hyp_puthex(probe_word);
    hyp_putc('\n');
  }

  /* Place each guest image at its host PA. The IPC producer + consumer run the
   * SAME image (role chosen by x0) but at separate private host RAM regions. */
  uint64_t vm1_size = (uint64_t)(__guest_blob_end - __guest_blob_start);
  uint64_t vm2_size = (uint64_t)(__guest2_blob_end - __guest2_blob_start);
  uint64_t ipc_size = (uint64_t)(__ipc_blob_end - __ipc_blob_start);
  uint64_t dom0_size = (uint64_t)(__dom0_blob_end - __dom0_blob_start);
  uint64_t vmtgt_size = (uint64_t)(__vmtgt_blob_end - __vmtgt_blob_start);
  hyp_copy_image(GUEST_ENTRY_IPA, __guest_blob_start, vm1_size);
  hyp_copy_image(VM2_HOST_RAM_BASE, __guest2_blob_start, vm2_size);
  hyp_copy_image(IPCP_HOST_RAM_BASE, __ipc_blob_start, ipc_size);
  hyp_copy_image(IPCC_HOST_RAM_BASE, __ipc_blob_start, ipc_size);
  hyp_copy_image(DOM0_HOST_RAM_BASE, __dom0_blob_start, dom0_size);
  hyp_copy_image(VMTGT_HOST_RAM_BASE, __vmtgt_blob_start, vmtgt_size);
  uint64_t crash_size = (uint64_t)(__crasher_blob_end - __crasher_blob_start);
  hyp_copy_image(CRASH_HOST_RAM_BASE, __crasher_blob_start, crash_size);
  uint64_t hang_size = (uint64_t)(__hang_blob_end - __hang_blob_start);
  hyp_copy_image(HANG_HOST_RAM_BASE, __hang_blob_start, hang_size);
  uint64_t rng_size = (uint64_t)(__rng_blob_end - __rng_blob_start);
  hyp_copy_image(RNG_HOST_RAM_BASE, __rng_blob_start, rng_size);
  uint64_t blk_size = (uint64_t)(__blk_blob_end - __blk_blob_start);
  hyp_copy_image(BLK_HOST_RAM_BASE, __blk_blob_start, blk_size);
  uint64_t net_size = (uint64_t)(__net_blob_end - __net_blob_start);
  hyp_copy_image(NET_HOST_RAM_BASE, __net_blob_start, net_size);
  uint64_t pci_size = (uint64_t)(__pci_blob_end - __pci_blob_start);
  hyp_copy_image(PCI_HOST_RAM_BASE, __pci_blob_start, pci_size);
  uint64_t smp_size = (uint64_t)(__smp_blob_end - __smp_blob_start);
  hyp_copy_image(SMP_HOST_RAM_BASE, __smp_blob_start, smp_size);
  uint64_t balloon_size = (uint64_t)(__balloon_blob_end - __balloon_blob_start);
  hyp_copy_image(BALLOON_HOST_RAM_BASE, __balloon_blob_start, balloon_size);
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
  uint64_t vmtgt_l1 = s2_build_vm2(VMTGT_HOST_RAM_BASE, VMTGT_RAM_SIZE);
  uint64_t crash_l1 = s2_build_vm2(CRASH_HOST_RAM_BASE, CRASH_RAM_SIZE);
  uint64_t hang_l1 = s2_build_vm2(HANG_HOST_RAM_BASE, HANG_RAM_SIZE);
  uint64_t rng_l1 = s2_build_vm2(RNG_HOST_RAM_BASE, RNG_RAM_SIZE);
  uint64_t blk_l1 = s2_build_vm2(BLK_HOST_RAM_BASE, BLK_RAM_SIZE);
  uint64_t net_l1 = s2_build_vm2(NET_HOST_RAM_BASE, NET_RAM_SIZE);
  uint64_t pci_l1 = s2_build_vm2(PCI_HOST_RAM_BASE, PCI_RAM_SIZE);
  uint64_t smp_l1 = s2_build_vm2(SMP_HOST_RAM_BASE, SMP_RAM_SIZE);
  uint64_t balloon_l1 = s2_build_vm2(BALLOON_HOST_RAM_BASE, BALLOON_RAM_SIZE);

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

  /* Migration target: idle stub (id 5, VMID 6); dom0 live-migrates guest2's
   * snapshot into it. Same 64 MiB ram_size as guest2 so the clone fits. */
  vcpu_t *vmtgt = vcpu_alloc("vmtgt", GUEST_ENTRY_IPA, s2_make_vttbr(vmtgt_l1, 6), 0,
                             __vmtgt_blob_start, VMTGT_HOST_RAM_BASE, vmtgt_size);
  vmtgt->ram_size = VMTGT_RAM_SIZE;

  /* crasher (id 6, VMID 7): deliberately faults to demonstrate per-VM fault
   * isolation — the hyp reboots only it, the other 6 VMs keep running. */
  vcpu_t *crasher = vcpu_alloc("crasher", GUEST_ENTRY_IPA, s2_make_vttbr(crash_l1, 7), 0,
                               __crasher_blob_start, CRASH_HOST_RAM_BASE, crash_size);
  crasher->ram_size = CRASH_RAM_SIZE;

  /* hangguest (id 7, VMID 8): livelocks to demonstrate the liveness watchdog —
   * the hyp reboots it when it stops petting, while the others run on. */
  vcpu_t *hang = vcpu_alloc("hangguest", GUEST_ENTRY_IPA, s2_make_vttbr(hang_l1, 8), 0,
                            __hang_blob_start, HANG_HOST_RAM_BASE, hang_size);
  hang->ram_size = HANG_RAM_SIZE;

  /* rngclient (id 8, VMID 9): drives the emulated virtio-mmio entropy device.
   * Its stage-2 leaves the virtio window 0x0A000000 invalid -> MMIO traps. */
  vcpu_t *rngc = vcpu_alloc("rngclient", GUEST_ENTRY_IPA, s2_make_vttbr(rng_l1, 9), 0,
                            __rng_blob_start, RNG_HOST_RAM_BASE, rng_size);
  rngc->ram_size = RNG_RAM_SIZE;

  /* blkclient (id 9, VMID 10): drives the emulated virtio-mmio block device. */
  vcpu_t *blkc = vcpu_alloc("blkclient", GUEST_ENTRY_IPA, s2_make_vttbr(blk_l1, 10), 0,
                            __blk_blob_start, BLK_HOST_RAM_BASE, blk_size);
  blkc->ram_size = BLK_RAM_SIZE;

  /* netclient (id 10, VMID 11): drives the emulated virtio-mmio net device. */
  vcpu_t *netc = vcpu_alloc("netclient", GUEST_ENTRY_IPA, s2_make_vttbr(net_l1, 11), 0,
                            __net_blob_start, NET_HOST_RAM_BASE, net_size);
  netc->ram_size = NET_RAM_SIZE;

  /* pciclient (id 11, VMID 12): enumerates the emulated virtual PCI bus. */
  vcpu_t *pcic = vcpu_alloc("pciclient", GUEST_ENTRY_IPA, s2_make_vttbr(pci_l1, 12), 0,
                            __pci_blob_start, PCI_HOST_RAM_BASE, pci_size);
  pcic->ram_size = PCI_RAM_SIZE;

  /* smpguest: a 2-vCPU SMP VM. Its TWO vCPUs share ONE stage-2 (the same VTTBR
   * / VMID), so the same IPA maps to the same host PA for both — a real SMP
   * memory model. The primary (id 12, VMID 13) is a normal vcpu_alloc; the
   * secondary (id 13) is created OFF via vcpu_alloc_secondary, inheriting the
   * primary's stage-2/group/image but with a distinct MPIDR affinity (Aff0=1).
   * The primary brings the secondary up with PSCI CPU_ON, then they ping-pong
   * an SGI the hyp software-routes between the siblings. */
  vcpu_t *smp0 = vcpu_alloc("smp-cpu0", GUEST_ENTRY_IPA, s2_make_vttbr(smp_l1, 13), 0,
                            __smp_blob_start, SMP_HOST_RAM_BASE, smp_size);
  smp0->ram_size = SMP_RAM_SIZE;
  vcpu_t *smp1 = vcpu_alloc_secondary(smp0, "smp-cpu1", 0x80000001ULL);
  (void)smp1; /* brought online by the primary's PSCI CPU_ON, not run yet */

  /* balloonclient (id 14, VMID 14): drives the emulated virtio-mmio memory-
   * balloon device — inflate (donate+zero pages) and deflate, self-driven by
   * the device's own retarget clock. */
  vcpu_t *blnc = vcpu_alloc("balloonclient", GUEST_ENTRY_IPA, s2_make_vttbr(balloon_l1, 14), 0,
                            __balloon_blob_start, BALLOON_HOST_RAM_BASE, balloon_size);
  blnc->ram_size = BALLOON_RAM_SIZE;

  hyp_puts("[HYP] 15 vCPUs created (incl. dom0, vmtgt, crasher, hangguest, rng/blk/net/pci clients, 2-vCPU SMP, balloon). Starting scheduler.\n");
  hyp_puts("--------------------------------------------------\n\n");

  snapshot_init();      /* reserve the VM snapshot slot */
  virtio_blk_init();    /* reserve the virtio-blk backing disk */
  virtio_balloon_init();/* init the memory-balloon device */
  vcpu_sched_init();  /* arm CNTHV scheduler tick */
  vcpu_run_first();   /* enter VM1 — does not return */
}
