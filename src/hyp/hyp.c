#include "hyp.h"
#include "exception.h" /* ESR_EC, EC_HVC_AARCH64, ESR_ISS_* */
#include "stage2.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "psci/psci.h"
#include "hvc/hvc.h"
#include "fdt.h" /* fdt_build (foreign-guest device tree) */
#include "gic/gic.h" /* gic_enable_irq, gic_ack_irq, gic_end_irq */
#include "blk/blk.h" /* blk_read/blk_write/capacity (host virtio-blk) */
#include "net/net.h" /* net_tx/net_rx_poll/net_get_mac/arp (host virtio-net) */
#include "utils/utils.h" /* ESUCCESS */
#include "mm/pmm/pmm.h"
#include "mm/mmu/mmu.h" /* PHYS_TO_VIRT */
#include "mmio/mmio.h"  /* mmio_read32 (host UART RX poll) */
#include "uart/uart.h"

/* FermiOS EL2 hypervisor — milestone 2: EL2 trap plumbing + self-test.
 *
 * Under VHE the host installed its own exception table via the redirected
 * VBAR_EL1 (== VBAR_EL2). This module owns a SEPARATE table (hyp_vector_table)
 * that reads the EL2-banked syndrome registers and will become the guest-exit
 * path in milestone 3. Here we prove it works end-to-end without a guest: swap
 * it into VBAR_EL2, execute an HVC from the host (EL2), confirm the exception
 * is captured with the right syndrome, then restore the host table. */

int hyp_at_el2(void) {
  uint64_t el;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(el));
  return (int)(((el >> 2) & 3) == 2);
}

/* Self-test handshake: set by hyp_dispatch when the deliberate HVC lands. */
static volatile int hyp_selftest_hit;
static volatile uint64_t hyp_selftest_esr;

void hyp_dispatch(uint64_t type, hyp_trap_frame_t *frame) {
  uint64_t ec = ESR_EC(frame->esr);

  if (type == HYP_EXC_SYNC && ec == EC_HVC_AARCH64) {
    /* Hypercall. ELR_EL2 already points at the instruction after the HVC,
     * so there is nothing to advance — just record and return. The real
     * hypercall dispatcher arrives in a later milestone. */
    hyp_selftest_hit = 1;
    hyp_selftest_esr = frame->esr;
    return;
  }

  /* Anything else reaching the EL2 table right now is unexpected (the host
   * has its own table; this one is only live around the self-test and, later,
   * while a guest runs). Decode loudly and park — returning could fault-loop. */
  uart_println("");
  uart_println("===== UNEXPECTED EL2 TRAP =====");
  uart_printf("  type=%u  ESR_EL2=%x  EC=%x\n", type, frame->esr, ec);
  uart_printf("  ELR_EL2=%x  FAR_EL2=%x  HPFAR_EL2=%x\n",
              frame->elr, frame->far, frame->hpfar);
  uart_println("  (hypervisor bug — parking)");
  uart_println("===============================");
  for (;;) {
    __asm__ __volatile__("wfi");
  }
}

void hyp_init(void) {
  if (!hyp_at_el2()) {
    uart_println("[HYP] not at EL2 (legacy EL1 boot) — hypervisor layer disabled");
    return;
  }

  uart_println("[HYP] EL2 VHE host detected — exercising dedicated EL2 vector table");

  /* The host installed its table through the VHE-redirected vbar_el1, so the
   * live VBAR_EL2 IS the host table — save it to restore afterwards. */
  uint64_t host_vbar;
  __asm__ __volatile__("mrs %0, vbar_el2" : "=r"(host_vbar));

  /* Mask IRQ + FIQ so the only exception that can reach our temporary table
   * is the synchronous HVC we are about to issue. */
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #3");

  uint64_t hyp_vbar = (uint64_t)(uintptr_t)hyp_vector_table;
  __asm__ __volatile__("msr vbar_el2, %0\n\tisb" ::"r"(hyp_vbar));

  hyp_selftest_hit = 0;
  /* HVC at EL2 is taken to EL2 via the Current-EL/SP_ELx synchronous vector. */
  __asm__ __volatile__("hvc #0" ::: "memory");

  /* Restore the host table and DAIF before doing anything else. */
  __asm__ __volatile__("msr vbar_el2, %0\n\tisb" ::"r"(host_vbar));
  __asm__ __volatile__("msr daif, %0" ::"r"(daif));

  if (hyp_selftest_hit) {
    uart_printf("[HYP] self-test OK: HVC routed to EL2 table, ESR_EL2=%x EC=%x (HVC)\n",
                hyp_selftest_esr, ESR_EC(hyp_selftest_esr));
  } else {
    uart_println("[HYP] self-test FAILED: HVC did not reach the EL2 table");
  }
  uart_printf("[HYP] host VBAR_EL2 restored (%x); EL2 trap plumbing live\n",
              host_vbar);
}

/* ---- Milestone 3: world switch + stage-2 + trivial EL1 guest ---- */

extern char guest_stub_start[];
extern char guest_stub_end[];

#define GUEST_ENTRY_IPA 0x40000000ULL
#define GUEST_RAM_SIZE  0x00200000ULL /* one 2 MiB stage-2 block */
#define GUEST_MARKER_IPA 0x40000800ULL
#define GUEST_MARKER_EXPECT 0xFEEDFACECAFEBABEULL

/* Clean a guest-RAM range to PoU/PoC and invalidate I-cache so the guest, with
 * its own caches initially off, fetches the bytes we just wrote. */
static void guest_sync_icache(uint64_t kva, uint64_t size) {
  uint64_t line = 64;
  for (uint64_t p = kva; p < kva + size; p += line) {
    __asm__ __volatile__("dc cvau, %0" ::"r"(p) : "memory");
  }
  __asm__ __volatile__("dsb ish" ::: "memory");
  for (uint64_t p = kva; p < kva + size; p += line) {
    __asm__ __volatile__("ic ivau, %0" ::"r"(p) : "memory");
  }
  __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
}

void hyp_run_smoke_guest(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M3 smoke test: launching trivial EL1 guest");

  /* 1. Back the guest's 2 MiB IPA window with contiguous physical RAM. */
  uintptr_t ram_phys = pmm_allocate_pages(GUEST_RAM_SIZE / 0x1000);
  if (!ram_phys) {
    uart_errorln("[HYP] smoke: failed to allocate guest RAM");
    return;
  }

  /* 2. Build a stage-2 mapping IPA 0x40000000 -> ram_phys (Normal-WB, exec). */
  static stage2_t s2;
  if (!stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram_phys, GUEST_RAM_SIZE, /*device=*/0);

  /* 3. Copy the guest stub into the start of guest RAM and make it coherent. */
  uint64_t ram_kva = PHYS_TO_VIRT((uint64_t)ram_phys);
  uint64_t stub_len = (uint64_t)(guest_stub_end - guest_stub_start);
  for (uint64_t i = 0; i < stub_len; i++) {
    ((volatile uint8_t *)ram_kva)[i] = ((volatile uint8_t *)guest_stub_start)[i];
  }
  /* Zero the marker slot so we can detect the guest writing it. */
  *(volatile uint64_t *)(ram_kva + (GUEST_MARKER_IPA - GUEST_ENTRY_IPA)) = 0;
  guest_sync_icache(ram_kva, GUEST_RAM_SIZE);

  /* 4. Set up the vCPU: enter at the stub, EL1h + DAIF masked (SPSR=0x3C5). */
  static vcpu_t v;
  for (int i = 0; i < 31; i++) {
    v.x[i] = 0;
  }
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5; /* EL1h, DAIF masked */
  v.sp_el1 = GUEST_ENTRY_IPA + GUEST_RAM_SIZE; /* top of guest RAM */
  v.vttbr = stage2_vttbr(&s2);
  v.vmid = 1;
  v.id = 0;
  v.name = "smoke";

  uart_printf("[HYP] entering guest: entry=%x VTTBR_EL2=%x stub_len=%u\n",
              v.pc, v.vttbr, stub_len);

  /* 5. World-switch into the guest; returns here on the guest's HVC. */
  vcpu_enter(&v);

  /* 6. Verify the exit. */
  uint64_t ec = ESR_EC(v.esr);
  uint64_t marker =
      *(volatile uint64_t *)(ram_kva + (GUEST_MARKER_IPA - GUEST_ENTRY_IPA));

  uart_printf("[HYP] guest exited: reason=%u ESR_EL2=%x EC=%x guest_pc=%x x2=%x\n",
              v.exit_reason, v.esr, ec, v.pc, v.x[2]);
  uart_printf("[HYP] guest RAM marker @%x = %x (expect %x)\n",
              GUEST_MARKER_IPA, marker, GUEST_MARKER_EXPECT);

  int ok = (v.exit_reason == HYP_EXC_SYNC) && (ec == EC_HVC_AARCH64) &&
           (v.x[2] == 0xBEEF) && (marker == GUEST_MARKER_EXPECT);
  if (ok) {
    uart_println("[HYP] M3 smoke test: PASS (stage-2 + world-switch + HVC exit)");
  } else {
    uart_println("[HYP] M3 smoke test: FAIL");
  }

  /* Release the guest RAM; the stage-2 tables are a static leak (one guest,
   * torn down at milestone 9). */
  pmm_free_pages(ram_phys, GUEST_RAM_SIZE / 0x1000);
}

/* ---- Milestone 4: EL2-physical-timer (CNTHP) time-slicing ---- */

extern char guest_spin_start[];
extern char guest_spin_end[];

#define HCR_IMO        (1ULL << 4)   /* route phys IRQ to EL2 */
#define HYP_CNTHP_PPI  26            /* EL2 physical timer PPI */
#define TIMESLICE_COUNT 5

/* Arm the EL2 physical timer to fire `ticks` from now (absolute CVAL). */
static void cnthp_arm(uint64_t ticks) {
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
  __asm__ __volatile__("msr cnthp_cval_el2, %0" ::"r"(now + ticks));
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(1ULL)); /* enable */
}

/* Disable the EL2 physical timer (deassert its level-triggered IRQ line). */
static void cnthp_disarm(void) {
  __asm__ __volatile__("msr cnthp_ctl_el2, %0\n\tisb" ::"r"(0ULL));
}

void hyp_run_timeslice_demo(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M4 demo: EL2-timer time-slicing a spinning EL1 guest");

  uintptr_t ram_phys = pmm_allocate_pages(GUEST_RAM_SIZE / 0x1000);
  if (!ram_phys) {
    uart_errorln("[HYP] M4: failed to allocate guest RAM");
    return;
  }

  static stage2_t s2;
  if (!stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram_phys, GUEST_RAM_SIZE, /*device=*/0);

  uint64_t ram_kva = PHYS_TO_VIRT((uint64_t)ram_phys);
  uint64_t spin_len = (uint64_t)(guest_spin_end - guest_spin_start);
  for (uint64_t i = 0; i < spin_len; i++) {
    ((volatile uint8_t *)ram_kva)[i] = ((volatile uint8_t *)guest_spin_start)[i];
  }
  guest_sync_icache(ram_kva, GUEST_RAM_SIZE);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) {
    v.x[i] = 0;
  }
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5; /* EL1h, DAIF masked — but EL2-routed IRQ ignores PSTATE.I */
  v.sp_el1 = GUEST_ENTRY_IPA + GUEST_RAM_SIZE;
  v.vttbr = stage2_vttbr(&s2);
  v.hcr_extra = HCR_IMO; /* route physical IRQ to EL2 so the timer preempts */
  v.vmid = 1;
  v.id = 0;
  v.name = "spin";

  /* Frequency -> a ~5 ms slice. */
  uint64_t freq;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t slice = freq / 200; /* 5 ms */

  /* Enable the EL2 physical-timer PPI at the redistributor. The host's own
   * IRQs are masked for the whole demo, so this only fires into our world
   * switch (which runs with IMO so the IRQ exits the guest to EL2). */
  gic_enable_irq(HYP_CNTHP_PPI);

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3"); /* mask host IRQ+FIQ */

  uint64_t last_x10 = 0;
  int slices_done = 0;
  for (int s = 0; s < TIMESLICE_COUNT; s++) {
    cnthp_arm(slice);
    vcpu_enter(&v);

    /* Expect an IRQ exit. Ack on the physical interface, confirm it is our
     * EL2-timer PPI, disarm (deassert the level line), then EOI. */
    if (v.exit_reason == HYP_EXC_IRQ) {
      uint64_t intid = gic_ack_irq();
      cnthp_disarm();
      if (intid != GIC_INTID_NO_PENDING) {
        gic_end_irq(intid);
      }
      slices_done++;
      uart_printf("[HYP] slice %u: guest preempted at pc=%x, x10=%u (intid=%u)\n",
                  (uint64_t)(s + 1), v.pc, v.x[10], intid);
      last_x10 = v.x[10];
    } else {
      cnthp_disarm();
      uart_printf("[HYP] slice %u: unexpected exit reason=%u ESR=%x\n",
                  (uint64_t)(s + 1), v.exit_reason, v.esr);
      break;
    }
  }

  cnthp_disarm();
  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif)); /* restore host mask */

  int ok = (slices_done == TIMESLICE_COUNT) && (last_x10 > 0);
  if (ok) {
    uart_printf("[HYP] M4 demo: PASS (%u slices, guest counter reached %u)\n",
                (uint64_t)slices_done, last_x10);
  } else {
    uart_println("[HYP] M4 demo: FAIL");
  }

  pmm_free_pages(ram_phys, GUEST_RAM_SIZE / 0x1000);
}

/* ---- Milestone 7/8: boot the real (reduced-RAM) FermiOS as an EL1 guest ---- */

extern char __guest_blob_start[];
extern char __guest_blob_end[];

#define FERMI_GUEST_RAM (128ULL * 1024 * 1024) /* must match GUEST_MEM_SIZE */
/* UART_BASE comes from uart.h (0x09000000). */
#define ESR_EC_OF(esr)  ESR_EC(esr)

static const char *hyp_ec_name(uint64_t ec) {
  switch (ec) {
  case 0x01: return "WFx";
  case 0x16: return "HVC";
  case 0x17: return "SMC";
  case 0x18: return "trapped sysreg";
  case 0x20: return "instruction abort (lower EL / stage-2)";
  case 0x24: return "data abort (lower EL / stage-2)";
  default:   return "other";
  }
}

/* PCI windows the guest's pci.c / virtio drivers touch. We emulate them as
 * "no device present": reads return all-1s (vendor 0xFFFF), writes are dropped.
 * This lets the guest's PCI enumeration + virtio init cleanly find nothing and
 * proceed to its scheduler/shell. (Real device pass-through is a later step.) */
#define PCI_ECAM_IPA   0x4010000000ULL
#define PCI_ECAM_END   0x4020000000ULL /* 256 MiB ECAM */
#define PCI_MMIO32_IPA 0x10000000ULL
#define PCI_MMIO32_END 0x3F000000ULL
#define PCI_MMIO64_IPA 0x8000000000ULL
#define PCI_MMIO64_END 0x10000000000ULL

static int pci_absent_is_target(uint64_t ipa) {
  return (ipa >= PCI_ECAM_IPA && ipa < PCI_ECAM_END) ||
         (ipa >= PCI_MMIO32_IPA && ipa < PCI_MMIO32_END) ||
         (ipa >= PCI_MMIO64_IPA && ipa < PCI_MMIO64_END);
}

/* Decode a guest stage-2 data abort (ESR_EL2 ISS, ISV=1 single-register
 * load/store) and apply an emulated MMIO access. `read_val` supplies the value
 * for guest reads (masked to access width). Writes are dropped. Returns 1 if
 * handled, 0 if the syndrome can't be decoded (ISV=0). Advances v->pc on
 * success. */
static int hyp_emulate_mmio(vcpu_t *v, uint64_t read_val) {
  uint64_t iss = v->esr & 0x1FFFFFFULL;
  if (!((iss >> 24) & 1)) {
    return 0; /* ISV=0: no decoded syndrome */
  }
  int is_write = (int)((iss >> 6) & 1);
  int sas = (int)((iss >> 22) & 3);
  int srt = (int)((iss >> 16) & 0x1F);
  int sf = (int)((iss >> 15) & 1);
  uint32_t size_mask = (sas >= 2) ? 0xFFFFFFFFU : ((1U << (8 << sas)) - 1U);

  if (!is_write && srt != 31) {
    uint64_t val = read_val & size_mask;
    v->x[srt] = sf ? val : (val & 0xFFFFFFFFULL);
  }
  /* writes: dropped */
  v->pc += 4;
  return 1;
}

void hyp_boot_fermios_guest(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uint64_t blob_len = (uint64_t)(__guest_blob_end - __guest_blob_start);
  uart_printf("[HYP] M8: booting FermiOS-as-guest (blob %u bytes, %u MiB RAM)\n",
              blob_len, FERMI_GUEST_RAM / (1024 * 1024));

  /* Back the guest's RAM window with contiguous host RAM. */
  uintptr_t ram_phys = pmm_allocate_pages(FERMI_GUEST_RAM / 0x1000);
  if (!ram_phys) {
    uart_errorln("[HYP] M8: failed to allocate guest RAM");
    return;
  }

  static stage2_t s2;
  if (!stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  /* Guest RAM IPA 0x40000000 -> our PMM chunk (Normal-WB, executable). */
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram_phys, FERMI_GUEST_RAM, /*device=*/0);
  /* PL011 UART straight-through so the guest's prints appear directly. */
  stage2_map(&s2, UART_BASE, UART_BASE, 0x1000, /*device=*/1);

  /* Copy the flat guest image into the base of guest RAM. */
  uint64_t ram_kva = PHYS_TO_VIRT((uint64_t)ram_phys);
  for (uint64_t i = 0; i < blob_len; i++) {
    ((volatile uint8_t *)ram_kva)[i] = ((volatile uint8_t *)__guest_blob_start)[i];
  }
  guest_sync_icache(ram_kva, (blob_len + 0xFFF) & ~0xFFFULL);

  /* Enable the virtual GIC interface (one-time). */
  vgic_init();

  /* Enable the guest's EL1 physical-timer PPI (30) on the REAL redistributor.
   * The guest's own gic_enable_irq(30) only touched our vGIC software model, so
   * without this the physical CNTP IRQ never reaches EL2 to be injected. With
   * IMO routing it now exits the guest to EL2, where we inject vINTID 30. */
  gic_enable_irq(30);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) {
    v.x[i] = 0;
  }
  v.pc = GUEST_ENTRY_IPA; /* flat image: entry == RAM base (boot.S _start) */
  v.pstate = 0x3C5;       /* EL1h, DAIF masked (guest's boot.S runs MMU-off) */
  v.sp_el1 = 0;           /* guest boot.S sets its own SP */
  v.vttbr = stage2_vttbr(&s2);
  v.hcr_extra = HCR_IMO;  /* route phys IRQ to EL2 + enable virtual interface */
  v.vmid = 1;
  v.id = 0;
  v.name = "fermios";
  vgic_vcpu_reset(&v.vgic);
  vgic_set_current(&v.vgic);

  uart_printf("[HYP] entering FermiOS guest: entry=%x VTTBR_EL2=%x\n",
              v.pc, v.vttbr);
  uart_println("[HYP] ---- guest output follows ----");

  /* Run the guest, servicing GICD/GICR MMIO so its gic_init completes.
   * Bounded so a guest fault loop can't hang the host. */
  long max_exits = 100000;
  uint64_t mmio_count = 0;
  uint64_t irq_count = 0;
  while (max_exits-- > 0) {
    vcpu_enter(&v);
    uint64_t ec = ESR_EC_OF(v.esr);

    /* Stage-2 data abort: emulate vGIC MMIO and absent-PCI windows. */
    if (v.exit_reason == HYP_EXC_SYNC && ec == 0x24) {
      uint64_t ipa = ((v.hpfar >> 4) << 12) | (v.far & 0xFFF);

      if (vgic_mmio_is_target(ipa)) {
        uint64_t iss = v.esr & 0x1FFFFFFULL;
        if (!((iss >> 24) & 1)) {
          uart_printf("\n[HYP] vGIC MMIO ISV=0 at IPA=%x — cannot decode\n", ipa);
          break;
        }
        int is_write = (int)((iss >> 6) & 1);
        int srt = (int)((iss >> 16) & 0x1F);
        int sf = (int)((iss >> 15) & 1);
        int size_bytes = 1 << (int)((iss >> 22) & 3);
        uint64_t val = 0;
        if (is_write) {
          val = (srt == 31) ? 0 : v.x[srt];
          vgic_mmio_emulate(ipa, 1, &val, size_bytes);
        } else {
          vgic_mmio_emulate(ipa, 0, &val, size_bytes);
          if (srt != 31) {
            v.x[srt] = sf ? val : (val & 0xFFFFFFFFULL);
          }
        }
        v.pc += 4;
        mmio_count++;
        continue;
      }

      if (pci_absent_is_target(ipa)) {
        /* No device: reads see all-1s (vendor 0xFFFF), writes dropped. */
        if (hyp_emulate_mmio(&v, 0xFFFFFFFFFFFFFFFFULL)) {
          mmio_count++;
          continue;
        }
        uart_printf("\n[HYP] PCI MMIO ISV=0 at IPA=%x — cannot decode\n", ipa);
        break;
      }

      /* A real stage-2 fault elsewhere — report and stop. */
      uart_printf("\n[HYP] guest stage-2 data abort OUTSIDE emulated MMIO: IPA=%x pc=%x ESR=%x\n",
                  ipa, v.pc, v.esr);
      break;
    }

    /* Physical IRQ routed to EL2 while the guest ran. The only one we expect
     * here is the guest's own EL1 physical timer (PPI 30); ack it, inject the
     * virtual INTID 30 into the guest, EOI, and resume. */
    if (v.exit_reason == HYP_EXC_IRQ) {
      uint64_t intid = gic_ack_irq();
      if (intid != GIC_INTID_NO_PENDING) {
        if (intid == 30) {
          vgic_inject_ppi(30);
        }
        gic_end_irq(intid);
      }
      irq_count++;
      continue;
    }

    if (v.exit_reason == HYP_EXC_SYNC && ec == 0x16) {
      uart_printf("\n[HYP] guest HVC at pc=%x x0=%x — stopping observation\n",
                  v.pc, v.x[0]);
      break;
    }

    /* Any other trap: report richly and stop (defines the next wall). */
    uint64_t ipa = ((v.hpfar >> 4) << 12) | (v.far & 0xFFF);
    uart_printf("\n[HYP] guest TRAP: reason=%u EC=%x (%s) after %u vGIC MMIO ops\n",
                v.exit_reason, ec, hyp_ec_name(ec), mmio_count);
    uart_printf("       guest_pc=%x ESR_EL2=%x FAR_EL2=%x HPFAR_EL2=%x faultIPA=%x\n",
                v.pc, v.esr, v.far, v.hpfar, ipa);
    break;
  }

  uart_printf("[HYP] ---- end FermiOS guest observation (%u vGIC MMIO ops) ----\n",
              mmio_count);
  pmm_free_pages(ram_phys, FERMI_GUEST_RAM / 0x1000);
}

/* ---- Milestone 9: round-robin two heartbeat EL1 guests ---- */

extern char guest_hb_start[];
extern char guest_hb_end[];

#define MG_COUNT      2
#define MG_RAM_SIZE   0x00200000ULL  /* 2 MiB per guest */
#define MG_ROUNDS     5              /* beats per guest */
#define MG_MARKER_OFF 0x800          /* where the guest stores its counter */

void hyp_run_multi_guest_demo(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M9: round-robin two heartbeat EL1 guests (A and B)");

  static stage2_t s2[MG_COUNT];
  static vcpu_t vc[MG_COUNT];
  uintptr_t ram[MG_COUNT];
  uint64_t hb_len = (uint64_t)(guest_hb_end - guest_hb_start);

  for (int i = 0; i < MG_COUNT; i++) {
    ram[i] = pmm_allocate_pages(MG_RAM_SIZE / 0x1000);
    if (!ram[i]) {
      uart_errorln("[HYP] M9: guest RAM alloc failed");
      return;
    }
    if (!stage2_create(&s2[i], /*vmid=*/(uint32_t)(i + 1))) {
      return;
    }
    /* Each guest's IPA 0x40000000 maps to a DIFFERENT host PA -> isolation. */
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], MG_RAM_SIZE, 0);
    stage2_map(&s2[i], UART_BASE, UART_BASE, 0x1000, /*device=*/1);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < hb_len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_hb_start)[b];
    }
    *(volatile uint64_t *)(kva + MG_MARKER_OFF) = 0;
    guest_sync_icache(kva, (hb_len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) {
      vc[i].x[r] = 0;
    }
    vc[i].x[0] = (uint64_t)('A' + i); /* id char the heartbeat prints */
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;             /* EL1h, DAIF masked */
    vc[i].sp_el1 = GUEST_ENTRY_IPA + MG_RAM_SIZE;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    vc[i].hcr_extra = 0;              /* voluntary HVC yields; no IRQ routing */
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "guestA" : "guestB";
  }

  uart_puts("[HYP] schedule (each char = one beat of that guest): ");

  /* Round-robin: run a beat of each guest in turn. Each guest runs until its
   * voluntary HVC yield, at which point we advance its PC past the hvc and move
   * on. The world switch preserves each vCPU's full GP context (incl. its x10
   * counter and x0 id) and swaps VTTBR_EL2 (VMID + stage-2). */
  for (int round = 0; round < MG_ROUNDS; round++) {
    for (int i = 0; i < MG_COUNT; i++) {
      vcpu_enter(&vc[i]);
      uint64_t ec = ESR_EC_OF(vc[i].esr);
      /* HVC: ELR_EL2 already points past the hvc (at the `b 1b`), so the next
       * entry resumes the loop. Do NOT advance pc. */
      if (!(vc[i].exit_reason == HYP_EXC_SYNC && ec == 0x16)) {
        uart_printf("\n[HYP] guest %s unexpected exit reason=%u EC=%x pc=%x\n",
                    vc[i].name, vc[i].exit_reason, ec, vc[i].pc);
        return;
      }
    }
  }
  uart_println("");

  /* Verify per-vCPU isolation: each guest's counter (x10) should equal the
   * number of rounds, and each guest's RAM marker should match its own x10 and
   * the two RAM regions are physically distinct. */
  int ok = 1;
  for (int i = 0; i < MG_COUNT; i++) {
    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    uint64_t marker = *(volatile uint64_t *)(kva + MG_MARKER_OFF);
    uart_printf("[HYP] guest %s: x0(id)=%c x10(beats)=%u RAM@phys %x marker=%u\n",
                vc[i].name, (uint64_t)vc[i].x[0], vc[i].x[10],
                (uint64_t)ram[i], marker);
    if (vc[i].x[10] != MG_ROUNDS || marker != (uint64_t)MG_ROUNDS ||
        vc[i].x[0] != (uint64_t)('A' + i)) {
      ok = 0;
    }
  }
  if (ram[0] == ram[1]) {
    ok = 0;
  }

  uart_println(ok ? "[HYP] M9 demo: PASS (two isolated guests round-robined)"
                  : "[HYP] M9 demo: FAIL");

  for (int i = 0; i < MG_COUNT; i++) {
    pmm_free_pages(ram[i], MG_RAM_SIZE / 0x1000);
  }
}

/* ---- Milestone 9c: preemptively round-robin two full FermiOS guests ---- */

#define DF_COUNT     2
#define DF_RAM_SIZE  (128ULL * 1024 * 1024)
#define DF_SLICES    8         /* total scheduler slices to run */

/* Service a single guest exit that is NOT a scheduler tick (MMIO, guest timer,
 * HVC). Returns 1 to keep running this guest, 0 if it hit something fatal. */
static int df_service_exit(vcpu_t *v) {
  uint64_t ec = ESR_EC_OF(v->esr);

  if (v->exit_reason == HYP_EXC_SYNC && ec == 0x24) {
    uint64_t ipa = ((v->hpfar >> 4) << 12) | (v->far & 0xFFF);

    if (vuart_is_target(ipa)) {
      uint64_t iss = v->esr & 0x1FFFFFFULL;
      if (!((iss >> 24) & 1)) return 0;
      int is_write = (int)((iss >> 6) & 1);
      int srt = (int)((iss >> 16) & 0x1F);
      int sf = (int)((iss >> 15) & 1);
      int size_bytes = 1 << (int)((iss >> 22) & 3);
      uint64_t val = 0;
      if (is_write) {
        val = (srt == 31) ? 0 : v->x[srt];
        vuart_emulate(&v->vuart, ipa, 1, &val, size_bytes);
      } else {
        vuart_emulate(&v->vuart, ipa, 0, &val, size_bytes);
        if (srt != 31) v->x[srt] = sf ? val : (val & 0xFFFFFFFFULL);
      }
      v->pc += 4;
      return 1;
    }

    if (vgic_mmio_is_target(ipa)) {
      uint64_t iss = v->esr & 0x1FFFFFFULL;
      if (!((iss >> 24) & 1)) return 0;
      int is_write = (int)((iss >> 6) & 1);
      int srt = (int)((iss >> 16) & 0x1F);
      int sf = (int)((iss >> 15) & 1);
      int size_bytes = 1 << (int)((iss >> 22) & 3);
      uint64_t val = 0;
      if (is_write) {
        val = (srt == 31) ? 0 : v->x[srt];
        vgic_mmio_emulate(ipa, 1, &val, size_bytes);
      } else {
        vgic_mmio_emulate(ipa, 0, &val, size_bytes);
        if (srt != 31) v->x[srt] = sf ? val : (val & 0xFFFFFFFFULL);
      }
      v->pc += 4;
      return 1;
    }
    if (pci_absent_is_target(ipa)) {
      return hyp_emulate_mmio(v, 0xFFFFFFFFFFFFFFFFULL);
    }
    return 0; /* real stage-2 fault */
  }

  if (v->exit_reason == HYP_EXC_SYNC && ec == 0x16) {
    /* Guest HVC (e.g. PSCI). Ignore for the demo and resume past it
     * (ELR_EL2 already points past the hvc). */
    return 1;
  }

  /* Any other sync exit kills this guest's slice. */
  return 0;
}

/* Warm-reset a guest: reload its flat image into its RAM and reset the vCPU to
 * a fresh boot state. Used to service a guest PSCI SYSTEM_RESET. */
static void df_reset_guest(vcpu_t *v, uintptr_t ram_phys, uint64_t ram_size) {
  uint64_t blob_len = (uint64_t)(__guest_blob_end - __guest_blob_start);
  uint64_t kva = PHYS_TO_VIRT(ram_phys);
  for (uint64_t b = 0; b < blob_len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)__guest_blob_start)[b];
  }
  guest_sync_icache(kva, (blob_len + 0xFFF) & ~0xFFFULL);

  for (int r = 0; r < 31; r++) v->x[r] = 0;
  v->pc = GUEST_ENTRY_IPA;
  v->pstate = 0x3C5;
  v->sp_el1 = 0;
  vgic_vcpu_reset(&v->vgic);
  vuart_init(&v->vuart, v->name);
  (void)ram_size;
}

void hyp_run_dual_fermios(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M9c: preemptively round-robin TWO full FermiOS guests");

  static stage2_t s2[DF_COUNT];
  static vcpu_t vc[DF_COUNT];
  uintptr_t ram[DF_COUNT];
  uint64_t blob_len = (uint64_t)(__guest_blob_end - __guest_blob_start);

  vgic_init();
  gic_enable_irq(30); /* guest EL1 phys-timer PPI on the real redistributor */

  for (int i = 0; i < DF_COUNT; i++) {
    ram[i] = pmm_allocate_pages(DF_RAM_SIZE / 0x1000);
    if (!ram[i] || !stage2_create(&s2[i], (uint32_t)(i + 1))) {
      uart_errorln("[HYP] M9c: setup failed");
      return;
    }
    /* Guest RAM only. The PL011 UART is left stage-2-UNMAPPED so the guest's
     * console MMIO traps to the per-guest virtual UART (vuart) instead of the
     * shared physical device. */
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], DF_RAM_SIZE, 0);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < blob_len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)__guest_blob_start)[b];
    }
    guest_sync_icache(kva, (blob_len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) vc[i].x[r] = 0;
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;
    vc[i].sp_el1 = 0;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    vc[i].hcr_extra = HCR_IMO;
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "vm0" : "vm1";
    vgic_vcpu_reset(&vc[i].vgic);
    vuart_init(&vc[i].vuart, vc[i].name);
  }

  uint64_t freq;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t slice = freq / 50; /* 20 ms scheduler quantum */

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  /* First-run flag: a vCPU's EL1/FP context is only valid to restore after it
   * has run once. We restore-on-switch; the very first entry of each uses the
   * fresh reset state already in the hardware bank when it boots MMU-off. */
  int started[DF_COUNT] = {0, 0};
  int off[DF_COUNT] = {0, 0};
  int cur_id = 0;

  for (int s = 0; s < DF_SLICES; s++) {
    /* Skip powered-off guests; stop if all are off. */
    int tries = 0;
    while (off[cur_id] && tries < DF_COUNT) {
      cur_id = (cur_id + 1) % DF_COUNT;
      tries++;
    }
    if (tries >= DF_COUNT) {
      uart_println("[HYP] all guests powered off");
      break;
    }
    vcpu_t *v = &vc[cur_id];

    /* Restore this guest's saved context (skip the very first time it runs). */
    if (started[cur_id]) {
      vcpu_restore_el1(&v->el1);
      vcpu_restore_fp(&v->fp);
      vgic_restore(&v->vgic);
    }
    vgic_set_current(&v->vgic);

    uart_printf("\n[HYP] >>> slice %u: scheduling %s (pc=%x) >>>\n",
                (uint64_t)s, v->name, v->pc);

    /* Run this guest until the scheduler quantum (CNTHP, PPI 26) expires. */
    cnthp_arm(slice);
    int slice_done = 0;
    int fresh = 0; /* set if this guest was reset/off this slice (no save) */
    long guard = 200000;
    while (!slice_done && guard-- > 0) {
      vcpu_enter(v);
      if (v->exit_reason == HYP_EXC_IRQ) {
        uint64_t intid = gic_ack_irq();
        if (intid == HYP_CNTHP_PPI) {
          cnthp_disarm();
          gic_end_irq(intid);
          slice_done = 1;          /* quantum expired -> switch guests */
        } else {
          if (intid == 30) vgic_inject_ppi(30); /* guest's own timer */
          if (intid != GIC_INTID_NO_PENDING) gic_end_irq(intid);
        }
      } else if (v->exit_reason == HYP_EXC_SYNC &&
                 ESR_EC_OF(v->esr) == 0x16) {
        /* Guest HVC -> PSCI. ELR_EL2 already points past the hvc. */
        psci_action_t act = psci_handle(v);
        if (act == PSCI_ACT_RESET) {
          vuart_flush(&v->vuart);
          uart_printf("\n[HYP] %s requested PSCI SYSTEM_RESET -> warm reset\n",
                      v->name);
          df_reset_guest(v, ram[cur_id], DF_RAM_SIZE);
          started[cur_id] = 0; /* fresh boot: no saved context to restore */
          fresh = 1;
          slice_done = 1;
        } else if (act == PSCI_ACT_OFF) {
          vuart_flush(&v->vuart);
          uart_printf("\n[HYP] %s requested PSCI power-off\n", v->name);
          off[cur_id] = 1;
          fresh = 1;
          slice_done = 1;
        }
        /* PSCI_ACT_NONE: x0 set with the result; resume the guest. */
      } else if (!df_service_exit(v)) {
        uint64_t ipa = ((v->hpfar >> 4) << 12) | (v->far & 0xFFF);
        uart_printf("[HYP] %s fatal exit reason=%u EC=%x pc=%x IPA=%x\n",
                    v->name, v->exit_reason, ESR_EC_OF(v->esr), v->pc, ipa);
        slice_done = 1;
      }
    }
    cnthp_disarm();

    /* Flush any partial console line so it is attributed to this guest and
     * does not bleed into the next guest's output. */
    vuart_flush(&v->vuart);

    /* Save this guest's live context before switching away. After a reset or
     * power-off the in-struct state is already the one we want to keep, so we
     * only quiesce the shared virtual-GIC HW interface and skip the save. */
    if (!fresh) {
      vcpu_save_el1(&v->el1);
      vcpu_save_fp(&v->fp);
      vgic_save(&v->vgic);
      started[cur_id] = 1;
    } else {
      /* Quiesce ICH_HCR_EL2 for the next guest without overwriting v->vgic. */
      __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(0ULL));
    }

    cur_id = (cur_id + 1) % DF_COUNT; /* round-robin */
  }

  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uart_println("\n[HYP] M9c: both FermiOS guests time-sliced; returning to host");

  for (int i = 0; i < DF_COUNT; i++) {
    pmm_free_pages(ram[i], DF_RAM_SIZE / 0x1000);
  }
}

/* ---- Milestone 11: PSCI SYSTEM_RESET self-test ---- */

extern char guest_psci_start[];
extern char guest_psci_end[];

void hyp_run_psci_test(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M11: PSCI test — guest will request SYSTEM_RESET");

  uintptr_t ram_phys = pmm_allocate_pages(MG_RAM_SIZE / 0x1000);
  if (!ram_phys) {
    return;
  }
  static stage2_t s2;
  if (!stage2_create(&s2, /*vmid=*/3)) {
    return;
  }
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram_phys, MG_RAM_SIZE, 0);

  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram_phys);
  uint64_t len = (uint64_t)(guest_psci_end - guest_psci_start);
  for (uint64_t b = 0; b < len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_psci_start)[b];
  }
  *(volatile uint64_t *)(kva + MG_MARKER_OFF) = 0;
  guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = GUEST_ENTRY_IPA + MG_RAM_SIZE;
  v.vttbr = stage2_vttbr(&s2);
  v.vmid = 3;
  v.name = "psci";

  int did_reset = 0;
  uint64_t post_reset_x10 = 0;
  for (int beat = 0; beat < 8; beat++) {
    vcpu_enter(&v);
    if (!(v.exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v.esr) == 0x16)) {
      uart_printf("[HYP] psci test: unexpected exit reason=%u EC=%x\n",
                  v.exit_reason, ESR_EC_OF(v.esr));
      break;
    }
    psci_action_t act = psci_handle(&v);
    if (act == PSCI_ACT_RESET) {
      uart_printf("[HYP] psci test: guest requested SYSTEM_RESET at x10=%u; "
                  "warm-resetting\n", v.x[10]);
      /* Warm reset: reload image + reset vcpu, re-enter at entry with x0=1 to
       * mark this as the post-reset "second life" so it counts instead of
       * resetting again. */
      for (uint64_t b = 0; b < len; b++) {
        ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_psci_start)[b];
      }
      guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);
      for (int i = 0; i < 31; i++) v.x[i] = 0;
      v.x[0] = 1; /* life tag: second life */
      v.pc = GUEST_ENTRY_IPA;
      v.pstate = 0x3C5;
      v.sp_el1 = GUEST_ENTRY_IPA + MG_RAM_SIZE;
      did_reset = 1;
    } else if (did_reset) {
      /* Post-reset yields: the second life counts up normally. */
      post_reset_x10 = v.x[10];
    }
  }

  /* The guest reset once, then on its second life counted past 1 — proving the
   * warm reset re-ran it from entry with a cleared counter. */
  int ok = did_reset && (post_reset_x10 >= 2);
  uart_printf("[HYP] psci test: post-reset beats=%u -> %s\n", post_reset_x10,
              ok ? "PASS (guest warm-reset + resumed)" : "FAIL");

  pmm_free_pages(ram_phys, MG_RAM_SIZE / 0x1000);
}

/* ---- Milestone 12: one interactive FermiOS guest ---- */

/* Non-blocking physical-UART read: returns -1 if no byte is pending. */
static int host_uart_rx_poll(void) {
  if (mmio_read32(UART_FR) & (1U << 4)) { /* RXFE: receive FIFO empty */
    return -1;
  }
  return (int)(mmio_read32(UART_DR) & 0xFF);
}

extern char guest_echo_start[];
extern char guest_echo_end[];

/* Back the guest's PCI ECAM bus-0 window (1 MiB at 0x4010000000) with a host
 * page region filled with 0xFF, stage-2-mapped Normal memory. The guest's PCI
 * enumeration then reads "no device" (vendor 0xFFFF) directly from RAM with NO
 * per-access trap — turning a ~65K-trap scan into a handful of cached reads.
 * Returns the backing phys (so it can be freed), or 0 on failure. */
#define ECAM_BUS0_IPA   0x4010000000ULL
#define ECAM_BUS0_SIZE  0x00200000ULL /* one 2 MiB stage-2 block (covers bus 0) */

static uintptr_t stage2_back_ecam(stage2_t *s2) {
  uintptr_t ecam = pmm_allocate_pages(ECAM_BUS0_SIZE / 0x1000);
  if (!ecam) {
    return 0;
  }
  volatile uint8_t *p = (volatile uint8_t *)PHYS_TO_VIRT((uint64_t)ecam);
  for (uint64_t i = 0; i < ECAM_BUS0_SIZE; i++) {
    p[i] = 0xFF; /* every config read => 0xFFFF... => "no device" */
  }
  __asm__ __volatile__("dsb ish" ::: "memory");
  stage2_map(s2, ECAM_BUS0_IPA, (uint64_t)ecam, ECAM_BUS0_SIZE, /*device=*/0);
  return ecam;
}

void hyp_run_interactive_guest(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M13: launching an interactive FermiOS guest");
  uart_println("[HYP] (host input is routed to the guest; output prefixed [vm])");

  uintptr_t ram_phys = pmm_allocate_pages(FERMI_GUEST_RAM / 0x1000);
  if (!ram_phys) {
    return;
  }
  static stage2_t s2;
  if (!stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  /* Guest RAM. UART left unmapped (traps to the per-guest vuart); ECAM bus 0
   * backed with 0xFF RAM so the PCI scan is trap-free. */
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram_phys, FERMI_GUEST_RAM, 0);
  uintptr_t ecam_phys = stage2_back_ecam(&s2);

  uint64_t len = (uint64_t)(__guest_blob_end - __guest_blob_start);
  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram_phys);
  for (uint64_t b = 0; b < len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)__guest_blob_start)[b];
  }
  guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = 0;
  v.vttbr = stage2_vttbr(&s2);
  v.hcr_extra = HCR_IMO; /* route the guest's timer (PPI 30) to EL2 */
  v.vmid = 1;
  v.name = "vm";
  vgic_init();
  vgic_vcpu_reset(&v.vgic);
  vgic_set_current(&v.vgic);
  vuart_init(&v.vuart, "vm");
  gic_enable_irq(30); /* guest EL1 phys-timer PPI on the real redistributor */

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  /* Full FermiOS guest: driven by its own timer (PPI 30, routed to EL2 and
   * injected as a vINTID) and by trap-emulated MMIO (vGIC, vUART, absent PCI).
   * On every exit we also pump pending host console input into the guest. */
  for (;;) {
    vcpu_enter(&v);

    int c;
    while ((c = host_uart_rx_poll()) >= 0) {
      vuart_rx_push(&v.vuart, (uint8_t)c);
    }

    if (v.exit_reason == HYP_EXC_IRQ) {
      uint64_t intid = gic_ack_irq();
      if (intid == 30) {
        /* Pass the guest's level-triggered EL1 timer through a HW-mapped LR:
         * the guest's virtual EOI will deactivate the physical IRQ. We must
         * NOT physically EOI it here, or the still-asserted level line re-fires
         * immediately and storms before the guest can re-arm CNTP_CVAL. */
        vgic_inject_hw(30);
      } else if (intid != GIC_INTID_NO_PENDING) {
        gic_end_irq(intid);
      }
      continue;
    }

    if (v.exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v.esr) == 0x16) {
      psci_action_t act = psci_handle(&v);
      if (act == PSCI_ACT_RESET) {
        vuart_flush(&v.vuart);
        uart_println("\n[HYP] guest reboot -> warm reset");
        df_reset_guest(&v, ram_phys, FERMI_GUEST_RAM);
        vgic_set_current(&v.vgic);
      } else if (act == PSCI_ACT_OFF) {
        uart_println("\n[HYP] guest powered off; returning to host");
        break;
      }
      continue;
    }

    if (!df_service_exit(&v)) {
      uint64_t ipa = ((v.hpfar >> 4) << 12) | (v.far & 0xFFF);
      uart_printf("\n[HYP] guest fatal exit reason=%u EC=%x pc=%x IPA=%x\n",
                  v.exit_reason, ESR_EC_OF(v.esr), v.pc, ipa);
      break;
    }
  }

  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  if (ecam_phys) {
    pmm_free_pages(ecam_phys, ECAM_BUS0_SIZE / 0x1000);
  }
  pmm_free_pages(ram_phys, FERMI_GUEST_RAM / 0x1000);
}

/* ---- Milestone 14: two FermiOS guests, preemptive AND interactive ---- */

#define MI_COUNT      2
#define MI_RAM_SIZE   (128ULL * 1024 * 1024)
#define MI_FOCUS_KEY  0x18  /* Ctrl-X cycles console focus between guests */

void hyp_run_multi_interactive(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M14: two preemptive + interactive FermiOS guests");
  uart_println("[HYP] (Ctrl-X switches which guest's shell receives your input)");

  static stage2_t s2[MI_COUNT];
  static vcpu_t vc[MI_COUNT];
  uintptr_t ram[MI_COUNT];
  uint64_t blob_len = (uint64_t)(__guest_blob_end - __guest_blob_start);

  vgic_init();

  for (int i = 0; i < MI_COUNT; i++) {
    ram[i] = pmm_allocate_pages(MI_RAM_SIZE / 0x1000);
    if (!ram[i] || !stage2_create(&s2[i], (uint32_t)(i + 1))) {
      uart_errorln("[HYP] M14: setup failed");
      return;
    }
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], MI_RAM_SIZE, 0);
    /* ECAM backed with 0xFF RAM for trap-free PCI scan; this loop never
     * returns, so the backing is intentionally kept for the VM's lifetime. */
    stage2_back_ecam(&s2[i]);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < blob_len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)__guest_blob_start)[b];
    }
    guest_sync_icache(kva, (blob_len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) vc[i].x[r] = 0;
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;
    vc[i].sp_el1 = 0;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    /* IMO routes physical IRQs to EL2 so the hypervisor's own scheduler tick
     * (CNTHP, PPI 26) preempts the guest even while it is in WFI. We do NOT
     * enable the physical guest timer (PPI 30); the hypervisor is the guest's
     * timer source, soft-injecting vINTID 30 per slice — so the two guests
     * never contend for the one physical timer or its Active state. */
    vc[i].hcr_extra = HCR_IMO;
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "vm0" : "vm1";
    vgic_vcpu_reset(&vc[i].vgic);
    vuart_init(&vc[i].vuart, vc[i].name);
  }

  /* Enable the EL2 physical-timer PPI (26) at the redistributor so the
   * scheduler quantum (CNTHP), with IMO routing it to EL2, actually preempts a
   * running/WFI guest and ends its slice. */
  gic_enable_irq(HYP_CNTHP_PPI);

  uint64_t freq;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t slice = freq / 60; /* ~16 ms scheduler quantum per guest */

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  int started[MI_COUNT] = {0, 0};
  int cur = 0, focus = 0;

  for (;;) {
    vcpu_t *v = &vc[cur];
    if (started[cur]) {
      vcpu_restore_el1(&v->el1);
      vcpu_restore_fp(&v->fp);
      vgic_restore(&v->vgic);
    }
    vgic_set_current(&v->vgic);

    /* Hypervisor IS the guest timer: present one virtual tick (INTID 30) per
     * slice so the guest's scheduler advances. Soft (HW=0) LR — the guest
     * virtually EOIs it, nothing physical to storm. Only inject once the guest
     * has enabled the timer PPI (past gic_init/sched_init), else we'd deliver a
     * tick before its run-queue exists and it would fault in sched_wake_sleepers. */
    if (vgic_intid_enabled(30)) {
      vgic_inject_ppi(30);
    }

    cnthp_arm(slice);
    int slice_done = 0;
    int fresh = 0; /* set if reset this slice -> don't save over fresh state */
    long guard = 2000000;
    while (!slice_done && guard-- > 0) {
      vcpu_enter(v);

      /* Pump host input to the FOCUSED guest; Ctrl-X cycles focus. */
      int c;
      while ((c = host_uart_rx_poll()) >= 0) {
        if (c == MI_FOCUS_KEY) {
          focus = (focus + 1) % MI_COUNT;
          uart_printf("\n[HYP] console focus -> %s\n", vc[focus].name);
        } else {
          vuart_rx_push(&vc[focus].vuart, (uint8_t)c);
        }
      }

      if (v->exit_reason == HYP_EXC_IRQ) {
        uint64_t intid = gic_ack_irq();
        if (intid == HYP_CNTHP_PPI) {
          cnthp_disarm();
          gic_end_irq(intid);
          slice_done = 1; /* quantum expired -> round-robin */
        } else if (intid != GIC_INTID_NO_PENDING) {
          gic_end_irq(intid);
        }
      } else if (v->exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v->esr) == 0x16) {
        psci_action_t act = psci_handle(v);
        if (act == PSCI_ACT_RESET) {
          vuart_flush(&v->vuart);
          uart_printf("\n[HYP] %s reboot -> warm reset\n", v->name);
          df_reset_guest(v, ram[cur], MI_RAM_SIZE);
          started[cur] = 0;
          fresh = 1;
          slice_done = 1;
        } else if (act == PSCI_ACT_OFF) {
          uart_printf("\n[HYP] %s powered off\n", v->name);
          slice_done = 1;
        }
      } else if (!df_service_exit(v)) {
        uint64_t ipa = ((v->hpfar >> 4) << 12) | (v->far & 0xFFF);
        uart_printf("\n[HYP] %s fatal exit reason=%u EC=%x pc=%x IPA=%x\n",
                    v->name, v->exit_reason, ESR_EC_OF(v->esr), v->pc, ipa);
        slice_done = 1;
      }
    }
    cnthp_disarm();

    vuart_flush(&v->vuart);
    if (!fresh) {
      /* Save the live guest context out before switching away. */
      vcpu_save_el1(&v->el1);
      vcpu_save_fp(&v->fp);
      vgic_save(&v->vgic);
      started[cur] = 1;
    } else {
      /* Just reset: keep the fresh in-struct state, only quiesce the shared
       * virtual-GIC HW interface for the next guest. */
      __asm__ __volatile__("msr ich_hcr_el2, %0\n\tisb" ::"r"(0ULL));
    }

    cur = (cur + 1) % MI_COUNT;
  }

  /* not reached */
}

/* ---- Milestone 15: inter-VM shared memory + doorbell hypercall ---- */

extern char guest_shm_start[];
extern char guest_shm_end[];

#define SHM_COUNT      2
#define SHM_RAM_SIZE   0x00200000ULL  /* 2 MiB per guest */
#define SHM_IPA        0x50000000ULL  /* shared page IPA (outside guest RAM) */
#define SHM_DOORBELL   0xF0000001ULL  /* doorbell hypercall function ID */
#define SHM_ROUNDS     6

void hyp_run_shm_doorbell(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M15: inter-VM shared memory + doorbell");

  /* One shared host page, mapped into BOTH guests at the same IPA. */
  uintptr_t shared = pmm_allocate_page();
  if (!shared) {
    return;
  }
  uint64_t *shared_kva = (uint64_t *)PHYS_TO_VIRT((uint64_t)shared);
  shared_kva[0] = 0; /* producer counter */
  shared_kva[1] = 0; /* doorbell seq     */
  __asm__ __volatile__("dsb ish" ::: "memory");

  static stage2_t s2[SHM_COUNT];
  static vcpu_t vc[SHM_COUNT];
  uintptr_t ram[SHM_COUNT];
  uint64_t len = (uint64_t)(guest_shm_end - guest_shm_start);

  for (int i = 0; i < SHM_COUNT; i++) {
    ram[i] = pmm_allocate_pages(SHM_RAM_SIZE / 0x1000);
    if (!ram[i] || !stage2_create(&s2[i], (uint32_t)(i + 1))) {
      return;
    }
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], SHM_RAM_SIZE, 0);
    stage2_map(&s2[i], UART_BASE, UART_BASE, 0x1000, /*device=*/1);
    /* The SAME physical page mapped into both guests at SHM_IPA = shared RAM. */
    stage2_map(&s2[i], SHM_IPA, (uint64_t)shared, 0x1000, /*device=*/0);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_shm_start)[b];
    }
    guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) vc[i].x[r] = 0;
    vc[i].x[0] = (uint64_t)i; /* role: 0 = producer, 1 = consumer */
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;
    vc[i].sp_el1 = GUEST_ENTRY_IPA + SHM_RAM_SIZE;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    vc[i].hcr_extra = 0;
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "producer" : "consumer";
  }

  uart_puts("[HYP] schedule (P=producer ran, C<hex>=consumer read shared counter): ");

  uint64_t doorbells = 0;
  for (int round = 0; round < SHM_ROUNDS; round++) {
    for (int i = 0; i < SHM_COUNT; i++) {
      /* Select this guest's vGIC model before running it, so a guest MMIO trap
       * into the (emulated) GICD/GICR window is serviced against the right
       * per-vCPU state rather than a stale/NULL `cur`. */
      vgic_set_current(&vc[i].vgic);
      /* Each guest runs until it yields via a plain HVC; the producer also
       * issues a doorbell HVC which we count + acknowledge. */
      int yielded = 0;
      long guard = 100000;
      while (!yielded && guard-- > 0) {
        vcpu_enter(&vc[i]);
        if (vc[i].exit_reason == HYP_EXC_SYNC && ESR_EC_OF(vc[i].esr) == 0x16) {
          if (vc[i].x[0] == SHM_DOORBELL) {
            /* Doorbell from the producer: in a full design this injects an SPI
             * into the peer VM's vGIC; here we count it and let the peer pick
             * up the data on its next slice (it polls shared memory). */
            doorbells++;
            vc[i].x[0] = 0; /* return success to the guest */
          } else {
            yielded = 1; /* plain yield HVC -> next guest */
          }
        } else if (!df_service_exit(&vc[i])) {
          yielded = 1;
        }
      }
    }
  }

  uint64_t final_counter = shared_kva[0];
  uint64_t final_seq = shared_kva[1];
  uart_println("");
  uart_printf("[HYP] shared counter=%u doorbell_seq=%u doorbells_serviced=%u\n",
              final_counter, final_seq, doorbells);
  int ok = (final_counter >= 1) && (doorbells >= 1) && (final_seq == final_counter);
  uart_println(ok ? "[HYP] M15: PASS (cross-VM shared memory + doorbell)"
                  : "[HYP] M15: FAIL");

  pmm_free_page(shared);
  for (int i = 0; i < SHM_COUNT; i++) {
    pmm_free_pages(ram[i], SHM_RAM_SIZE / 0x1000);
  }
}

/* ---- Milestone 16: interrupt-driven inter-VM doorbell ---- */

extern char guest_db_start[];
extern char guest_db_end[];
extern char guest_db_vectors_end[];

#define DB_DOORBELL_SPI 40   /* virtual SPI delivered to the consumer */

void hyp_run_doorbell_irq(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M16: interrupt-driven inter-VM doorbell (SPI injection)");

  uintptr_t shared = pmm_allocate_page();
  if (!shared) {
    return;
  }
  uint64_t *shared_kva = (uint64_t *)PHYS_TO_VIRT((uint64_t)shared);
  shared_kva[0] = 0; /* counter */
  shared_kva[1] = 0; /* doorbell seq */
  shared_kva[2] = 0; /* consumer IRQ-handler run count */
  __asm__ __volatile__("dsb ish" ::: "memory");

  static stage2_t s2[SHM_COUNT];
  static vcpu_t vc[SHM_COUNT];
  uintptr_t ram[SHM_COUNT];
  /* The whole blob spans guest_db_start .. guest_db_vectors_end (code + the
   * 2 KiB-aligned vector table). */
  uint64_t len = (uint64_t)(guest_db_vectors_end - guest_db_start);

  vgic_init();
  gic_enable_irq(HYP_CNTHP_PPI);

  for (int i = 0; i < SHM_COUNT; i++) {
    ram[i] = pmm_allocate_pages(SHM_RAM_SIZE / 0x1000);
    if (!ram[i] || !stage2_create(&s2[i], (uint32_t)(i + 1))) {
      return;
    }
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], SHM_RAM_SIZE, 0);
    stage2_map(&s2[i], UART_BASE, UART_BASE, 0x1000, /*device=*/1);
    stage2_map(&s2[i], SHM_IPA, (uint64_t)shared, 0x1000, /*device=*/0);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_db_start)[b];
    }
    guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) vc[i].x[r] = 0;
    vc[i].x[0] = (uint64_t)i; /* 0 = producer, 1 = consumer */
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;
    vc[i].sp_el1 = GUEST_ENTRY_IPA + SHM_RAM_SIZE;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    vc[i].hcr_extra = HCR_IMO; /* CNTHP preempts the WFI consumer */
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "producer" : "consumer";
    vgic_vcpu_reset(&vc[i].vgic);
    vuart_init(&vc[i].vuart, vc[i].name);
  }

  uint64_t freq;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t slice = freq / 200; /* 5 ms quantum */

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  int started[SHM_COUNT] = {0, 0};
  uint64_t doorbells = 0;
  /* Run enough slices for several producer->consumer rounds. */
  for (int s = 0; s < 12; s++) {
    int cur = s % SHM_COUNT;
    vcpu_t *v = &vc[cur];
    if (started[cur]) {
      vcpu_restore_el1(&v->el1);
      vcpu_restore_fp(&v->fp);
      vgic_restore(&v->vgic);
    }
    vgic_set_current(&v->vgic);

    cnthp_arm(slice);
    int slice_done = 0;
    long guard = 200000;
    while (!slice_done && guard-- > 0) {
      vcpu_enter(v);
      if (v->exit_reason == HYP_EXC_IRQ) {
        uint64_t intid = gic_ack_irq();
        if (intid == HYP_CNTHP_PPI) {
          cnthp_disarm();
          gic_end_irq(intid);
          slice_done = 1;
        } else if (intid != GIC_INTID_NO_PENDING) {
          gic_end_irq(intid);
        }
      } else if (v->exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v->esr) == 0x16) {
        if (v->x[0] == SHM_DOORBELL) {
          /* Producer rang the doorbell: inject the virtual SPI into the
           * CONSUMER's saved vGIC so it is presented when the consumer runs. */
          doorbells++;
          vgic_inject_to(&vc[1].vgic, DB_DOORBELL_SPI);
          v->x[0] = 0;
        }
        /* plain yield HVC or post-doorbell HVC: end the producer's slice */
        slice_done = 1;
      } else if (!df_service_exit(v)) {
        slice_done = 1;
      }
    }
    cnthp_disarm();
    vuart_flush(&v->vuart);
    vcpu_save_el1(&v->el1);
    vcpu_save_fp(&v->fp);
    vgic_save(&v->vgic);
    started[cur] = 1;
  }

  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uint64_t handled = shared_kva[2]; /* consumer bumps this in its IRQ handler */
  uart_println("");
  uart_printf("[HYP] doorbells rung=%u, consumer IRQ-handler runs=%u\n",
              doorbells, handled);
  int ok = (doorbells >= 1) && (handled >= 1);
  uart_println(ok ? "[HYP] M16: PASS (peer-injected SPI delivered to guest IRQ handler)"
                  : "[HYP] M16: FAIL");

  pmm_free_page(shared);
  for (int i = 0; i < SHM_COUNT; i++) {
    pmm_free_pages(ram[i], SHM_RAM_SIZE / 0x1000);
  }
}

/* ---- Milestone 17: unified HVC hypercall ABI demo ---- */

extern char guest_hvc_start[];
extern char guest_hvc_end[];

#define HVC_COUNT     2
#define HVC_RAM_SIZE  0x00200000ULL
#define HVC_ROUNDS    3

void hyp_run_hvc_abi(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M17: unified HVC hypercall ABI (VERSION/VM_INFO/PUTC/YIELD)");

  static stage2_t s2[HVC_COUNT];
  static vcpu_t vc[HVC_COUNT];
  uintptr_t ram[HVC_COUNT];
  uint64_t len = (uint64_t)(guest_hvc_end - guest_hvc_start);
  hvc_env_t env = {.vm_count = HVC_COUNT};

  for (int i = 0; i < HVC_COUNT; i++) {
    ram[i] = pmm_allocate_pages(HVC_RAM_SIZE / 0x1000);
    if (!ram[i] || !stage2_create(&s2[i], (uint32_t)(i + 1))) {
      return;
    }
    stage2_map(&s2[i], GUEST_ENTRY_IPA, (uint64_t)ram[i], HVC_RAM_SIZE, 0);

    uint64_t kva = PHYS_TO_VIRT((uint64_t)ram[i]);
    for (uint64_t b = 0; b < len; b++) {
      ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_hvc_start)[b];
    }
    guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

    for (int r = 0; r < 31; r++) vc[i].x[r] = 0;
    vc[i].pc = GUEST_ENTRY_IPA;
    vc[i].pstate = 0x3C5;
    vc[i].sp_el1 = GUEST_ENTRY_IPA + HVC_RAM_SIZE;
    vc[i].vttbr = stage2_vttbr(&s2[i]);
    vc[i].hcr_extra = 0;
    vc[i].vmid = (uint32_t)(i + 1);
    vc[i].id = (uint32_t)i;
    vc[i].name = (i == 0) ? "vm0" : "vm1";
    vuart_init(&vc[i].vuart, vc[i].name);
  }

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  /* Cooperative: each guest runs until it YIELDs (or makes a non-yield HVC we
   * handle in place and resume). Round-robin for a few rounds. */
  for (int round = 0; round < HVC_ROUNDS; round++) {
    for (int i = 0; i < HVC_COUNT; i++) {
      vcpu_t *v = &vc[i];
      int yielded = 0;
      long guard = 100000;
      while (!yielded && guard-- > 0) {
        vcpu_enter(v);
        if (v->exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v->esr) == 0x16) {
          hvc_action_t act = hvc_dispatch(v, &env);
          if (act == HVC_ACT_YIELD) {
            yielded = 1; /* end slice -> next guest */
          }
          /* HVC_ACT_NONE (VERSION/VM_INFO/PUTC): x0/x1 set, resume the guest. */
        } else if (!df_service_exit(v)) {
          yielded = 1;
        }
      }
      vuart_flush(&v->vuart);
    }
  }

  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uart_println("[HYP] M17: done (each guest printed 'H<vmid>' via the PUTC hypercall)");

  for (int i = 0; i < HVC_COUNT; i++) {
    pmm_free_pages(ram[i], HVC_RAM_SIZE / 0x1000);
  }
}

/* ---- Milestone 19: paravirtualized block device over hypercalls ---- */

extern char guest_blk_start[];
extern char guest_blk_end[];

#define PV_RAM_SIZE  0x00200000ULL

/* Service a BLK hypercall against the running guest `s2`. The guest buffer is
 * named by an IPA which we stage-2-translate (so the guest can only ever target
 * memory it actually owns), bounce through a host-side buffer, and drive the
 * real virtio-blk device. Returns the value to put in the guest's x0. */
static uint64_t pv_blk_service(vcpu_t *v, stage2_t *s2) {
  static uint8_t bounce[VIRTIO_BLK_SECTOR_SIZE] __attribute__((aligned(16)));
  uint64_t fn = v->x[0];

  if (fn == HVC_FN_BLK_INFO) {
    extern struct virtio_blk blk_dev;
    return blk_dev.capacity_sectors;
  }

  /* HVC_FN_BLK_RW: x1=sector, x2=guest buffer IPA, x3=write? */
  uint64_t sector = v->x[1];
  uint64_t buf_ipa = v->x[2];
  int is_write = (int)(v->x[3] & 1);

  /* Translate + bounds-check the guest buffer: the whole 512-byte sector must
   * lie within one mapped guest page (no straddling / unmapped access). */
  uint64_t buf_pa = stage2_translate(s2, buf_ipa);
  if (!buf_pa || (buf_ipa & 0xFFF) + VIRTIO_BLK_SECTOR_SIZE > 0x1000) {
    return (uint64_t)-1; /* unmapped or straddles a page -> reject */
  }
  uint8_t *buf_kva = (uint8_t *)PHYS_TO_VIRT(buf_pa);

  if (is_write) {
    for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) bounce[i] = buf_kva[i];
    return (blk_write(sector, bounce) == ESUCCESS) ? 0 : (uint64_t)-1;
  }
  if (blk_read(sector, bounce) != ESUCCESS) {
    return (uint64_t)-1;
  }
  for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) buf_kva[i] = bounce[i];
  __asm__ __volatile__("dsb ish" ::: "memory");
  return 0;
}

void hyp_run_blk_pv(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M19: paravirt block device (guest reads real disk via HVC)");

  uintptr_t ram = pmm_allocate_pages(PV_RAM_SIZE / 0x1000);
  static stage2_t s2;
  if (!ram || !stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram, PV_RAM_SIZE, 0);

  uint64_t len = (uint64_t)(guest_blk_end - guest_blk_start);
  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram);
  for (uint64_t b = 0; b < len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_blk_start)[b];
  }
  guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = GUEST_ENTRY_IPA + PV_RAM_SIZE;
  v.vttbr = stage2_vttbr(&s2);
  v.vmid = 1;
  v.name = "blkvm";
  vuart_init(&v.vuart, "blkvm");

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  uart_puts("[HYP] guest read of sector 0 (first 8 bytes): ");

  long guard = 100000;
  int done = 0;
  while (!done && guard-- > 0) {
    vcpu_enter(&v);
    if (v.exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v.esr) == 0x16) {
      uint64_t fn = v.x[0];
      if (fn == HVC_FN_BLK_INFO || fn == HVC_FN_BLK_RW) {
        v.x[0] = pv_blk_service(&v, &s2);
      } else if (fn == HVC_FN_PUTC) {
        vuart_emulate(&v.vuart, 0x09000000ULL, /*is_write=*/1, &v.x[1], 1);
        v.x[0] = 0;
      } else {
        /* plain yield HVC: the guest has finished its read+print loop. */
        done = 1;
      }
    } else if (!df_service_exit(&v)) {
      done = 1;
    }
  }

  vuart_flush(&v.vuart);
  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uart_println("[HYP] M19: done (bytes above are real on-disk data via stage-2-translated DMA)");

  pmm_free_pages(ram, PV_RAM_SIZE / 0x1000);
}

/* ---- Milestone 20: paravirtualized network device over hypercalls ---- */

extern char guest_net_start[];
extern char guest_net_end[];

#define NET_FRAME_MAX 1600

/* Service a NET hypercall against the running guest `s2`. Guest frame buffers
 * are named by IPA and stage-2-translated (the same isolation check as block).
 * Returns the value for the guest's x0. */
static uint64_t pv_net_service(vcpu_t *v, stage2_t *s2) {
  static uint8_t bounce[NET_FRAME_MAX] __attribute__((aligned(16)));
  uint64_t fn = v->x[0];

  if (fn == HVC_FN_NET_MAC) {
    uint8_t mac[6] = {0};
    if (!net_get_mac(mac)) {
      return (uint64_t)-1;
    }
    uint64_t packed = 0;
    for (int i = 0; i < 6; i++) {
      packed |= (uint64_t)mac[i] << (8 * i);
    }
    return packed;
  }

  if (fn == HVC_FN_NET_ARP) {
    /* The hypervisor builds + sends the ARP probe on the guest's behalf. */
    return (net_send_arp_probe() > 0) ? 0 : (uint64_t)-1;
  }

  if (fn == HVC_FN_NET_TX) {
    uint64_t ipa = v->x[1];
    uint64_t fl = v->x[2];
    if (fl == 0 || fl > NET_FRAME_MAX) {
      return (uint64_t)-1;
    }
    uint64_t pa = stage2_translate(s2, ipa);
    if (!pa || (ipa & 0xFFF) + fl > 0x1000) {
      return (uint64_t)-1; /* unmapped or page-straddling */
    }
    uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(pa);
    for (uint64_t i = 0; i < fl; i++) bounce[i] = kva[i];
    return (net_tx(bounce, (uint32_t)fl) > 0) ? 0 : (uint64_t)-1;
  }

  if (fn == HVC_FN_NET_RX) {
    uint64_t ipa = v->x[1];
    uint64_t maxlen = v->x[2];
    if (maxlen == 0) {
      return 0;
    }
    if (maxlen > NET_FRAME_MAX) {
      maxlen = NET_FRAME_MAX;
    }
    uint64_t pa = stage2_translate(s2, ipa);
    if (!pa || (ipa & 0xFFF) + maxlen > 0x1000) {
      return (uint64_t)-1;
    }
    int n = net_rx_poll(bounce, (uint32_t)maxlen);
    if (n <= 0) {
      return 0; /* nothing received */
    }
    uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(pa);
    for (int i = 0; i < n; i++) kva[i] = bounce[i];
    __asm__ __volatile__("dsb ish" ::: "memory");
    return (uint64_t)n;
  }

  return (uint64_t)-1;
}

void hyp_run_net_pv(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M20: paravirt network (guest does an ARP round-trip via HVC)");

  uintptr_t ram = pmm_allocate_pages(PV_RAM_SIZE / 0x1000);
  static stage2_t s2;
  if (!ram || !stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram, PV_RAM_SIZE, 0);

  uint64_t len = (uint64_t)(guest_net_end - guest_net_start);
  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram);
  for (uint64_t b = 0; b < len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_net_start)[b];
  }
  guest_sync_icache(kva, (len + 0xFFF) & ~0xFFFULL);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = GUEST_ENTRY_IPA + PV_RAM_SIZE;
  v.vttbr = stage2_vttbr(&s2);
  v.vmid = 1;
  v.name = "netvm";
  vuart_init(&v.vuart, "netvm");

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  uart_puts("[HYP] guest net result (M<mac> then R<rxlen> or T=timeout): ");

  long guard = 4000000; /* the guest spins polling RX; allow generous budget */
  int done = 0;
  while (!done && guard-- > 0) {
    vcpu_enter(&v);
    if (v.exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v.esr) == 0x16) {
      uint64_t fn = v.x[0];
      if (fn >= HVC_FN_NET_MAC && fn <= HVC_FN_NET_ARP) {
        v.x[0] = pv_net_service(&v, &s2);
      } else if (fn == HVC_FN_PUTC) {
        vuart_emulate(&v.vuart, 0x09000000ULL, /*is_write=*/1, &v.x[1], 1);
        v.x[0] = 0;
      } else {
        done = 1; /* plain yield: guest finished */
      }
    } else if (!df_service_exit(&v)) {
      done = 1;
    }
  }

  vuart_flush(&v.vuart);
  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uart_println("[HYP] M20: done (guest drove the real NIC: MAC query + ARP TX + RX poll)");

  pmm_free_pages(ram, PV_RAM_SIZE / 0x1000);
}

/* ---- Milestone 21: dynamic VM lifecycle (create / run / destroy) ---- */

#define VM_RAM_SIZE   0x00200000ULL  /* 2 MiB per VM */
#define VM_CYCLES     4
#define VM_BEATS      3

/* Create + run + destroy one heartbeat VM with the given VMID. Returns 1 on a
 * clean lifecycle. All resources (stage-2 tables, RAM, VMID) are released. */
static int vm_run_one(uint32_t vmid) {
  uintptr_t ram = pmm_allocate_pages(VM_RAM_SIZE / 0x1000);
  if (!ram) {
    return 0;
  }
  stage2_t s2;
  if (!stage2_create(&s2, vmid)) {
    pmm_free_pages(ram, VM_RAM_SIZE / 0x1000);
    return 0;
  }
  /* Map RAM + a straight-through UART, and force an L2+L3 table to exist (a
   * 4 KiB mapping) so teardown has real sub-tables to free. */
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram, VM_RAM_SIZE, 0);
  stage2_map(&s2, UART_BASE, UART_BASE, 0x1000, /*device=*/1);

  uint64_t hb_len = (uint64_t)(guest_hb_end - guest_hb_start);
  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram);
  for (uint64_t b = 0; b < hb_len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)guest_hb_start)[b];
  }
  guest_sync_icache(kva, (hb_len + 0xFFF) & ~0xFFFULL);

  vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.x[0] = (uint64_t)('0' + (vmid % 10)); /* heartbeat prints this id char */
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = GUEST_ENTRY_IPA + VM_RAM_SIZE;
  v.vttbr = stage2_vttbr(&s2);
  v.hcr_extra = 0;
  v.vmid = vmid;
  v.name = "vm";

  for (int beat = 0; beat < VM_BEATS; beat++) {
    vcpu_enter(&v);
    if (!(v.exit_reason == HYP_EXC_SYNC && ESR_EC_OF(v.esr) == 0x16)) {
      break; /* unexpected exit — still tear down cleanly below */
    }
  }

  /* Tear down: free the stage-2 page tables (+ TLBI), then the guest RAM. */
  stage2_destroy(&s2);
  pmm_free_pages(ram, VM_RAM_SIZE / 0x1000);
  return 1;
}

void hyp_run_vm_lifecycle(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M21: dynamic VM lifecycle (create/run/destroy, leak check)");

  uint64_t baseline = pmm_get_free_pages();
  int ok = 1;
  for (int c = 0; c < VM_CYCLES; c++) {
    if (!vm_run_one(/*vmid=*/(uint32_t)(c + 1))) {
      ok = 0;
      break;
    }
    uint64_t now = pmm_get_free_pages();
    uart_printf("[HYP]  cycle %u: free pages %u (baseline %u)\n",
                (uint64_t)(c + 1), now, baseline);
    if (now != baseline) {
      ok = 0; /* leaked (or, implausibly, gained) pages */
    }
  }

  uint64_t final = pmm_get_free_pages();
  uart_printf("[HYP] M21: %s (free pages %u, baseline %u after %u create/destroy cycles)\n",
              (ok && final == baseline) ? "PASS - no leak" : "FAIL - leak",
              final, baseline, (uint64_t)VM_CYCLES);
}

/* ---- Milestone 22: boot a foreign (non-FermiOS) guest via DTB ---- */

extern char __miniguest_blob_start[];
extern char __miniguest_blob_end[];

#define MG_FOREIGN_RAM  0x00200000ULL  /* 2 MiB */
#define MG_DTB_IPA      0x40100000ULL  /* DTB lives 1 MiB into guest RAM */

void hyp_run_miniguest(void) {
  if (!hyp_at_el2()) {
    return;
  }

  uart_println("[HYP] M22: booting a FOREIGN (non-FermiOS) guest via DTB");

  uintptr_t ram = pmm_allocate_pages(MG_FOREIGN_RAM / 0x1000);
  static stage2_t s2;
  if (!ram || !stage2_create(&s2, /*vmid=*/1)) {
    return;
  }
  /* RAM + a straight-through PL011 so the foreign guest's discovered UART
   * writes reach the real console directly. */
  stage2_map(&s2, GUEST_ENTRY_IPA, (uint64_t)ram, MG_FOREIGN_RAM, 0);
  stage2_map(&s2, UART_BASE, UART_BASE, 0x1000, /*device=*/1);

  uint64_t kva = PHYS_TO_VIRT((uint64_t)ram);

  /* Copy the standalone foreign-guest blob to the RAM base (IPA 0x40000000). */
  uint64_t blob_len = (uint64_t)(__miniguest_blob_end - __miniguest_blob_start);
  for (uint64_t b = 0; b < blob_len; b++) {
    ((volatile uint8_t *)kva)[b] = ((volatile uint8_t *)__miniguest_blob_start)[b];
  }

  /* Build a DTB into guest RAM at MG_DTB_IPA and self-check it. The guest sees
   * its RAM as [0x40000000, +2 MiB) and a PL011 at the architected UART base. */
  uint64_t dtb_kva = kva + (MG_DTB_IPA - GUEST_ENTRY_IPA);
  uint32_t dtb_sz = fdt_build((void *)dtb_kva, 0x1000, GUEST_ENTRY_IPA,
                              MG_FOREIGN_RAM, UART_BASE);
  if (!dtb_sz || !fdt_check((void *)dtb_kva)) {
    uart_errorln("[HYP] M22: DTB build failed");
    stage2_destroy(&s2);
    pmm_free_pages(ram, MG_FOREIGN_RAM / 0x1000);
    return;
  }
  uart_printf("[HYP] built DTB (%u bytes) at guest IPA %x\n",
              (uint64_t)dtb_sz, MG_DTB_IPA);
  guest_sync_icache(kva, MG_FOREIGN_RAM);

  static vcpu_t v;
  for (int i = 0; i < 31; i++) v.x[i] = 0;
  v.x[0] = MG_DTB_IPA;   /* AArch64 boot protocol: x0 = DTB address */
  v.pc = GUEST_ENTRY_IPA;
  v.pstate = 0x3C5;
  v.sp_el1 = 0;          /* the foreign guest sets its own SP */
  v.vttbr = stage2_vttbr(&s2);
  v.hcr_extra = HCR_IMO; /* so CNTHP can preempt its final WFI */
  v.vmid = 1;
  v.name = "miniguest";

  vgic_init();
  vgic_vcpu_reset(&v.vgic);
  vgic_set_current(&v.vgic);
  gic_enable_irq(HYP_CNTHP_PPI);

  uint64_t freq;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));

  uint64_t host_daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(host_daif));
  __asm__ __volatile__("msr daifset, #3");

  /* The foreign guest prints then WFIs; give it a few CNTHP-bounded slices to
   * run to completion, then stop. */
  for (int slice = 0; slice < 4; slice++) {
    cnthp_arm(freq / 100); /* 10 ms */
    int slice_done = 0;
    long guard = 200000;
    while (!slice_done && guard-- > 0) {
      vcpu_enter(&v);
      if (v.exit_reason == HYP_EXC_IRQ) {
        uint64_t intid = gic_ack_irq();
        if (intid == HYP_CNTHP_PPI) {
          cnthp_disarm();
          gic_end_irq(intid);
          slice_done = 1;
        } else if (intid != GIC_INTID_NO_PENDING) {
          gic_end_irq(intid);
        }
      } else if (!df_service_exit(&v)) {
        slice_done = 1;
      }
    }
    cnthp_disarm();
  }

  __asm__ __volatile__("msr daif, %0" ::"r"(host_daif));
  uart_println("[HYP] M22: done (a non-FermiOS guest booted + parsed our DTB)");

  stage2_destroy(&s2);
  pmm_free_pages(ram, MG_FOREIGN_RAM / 0x1000);
}
