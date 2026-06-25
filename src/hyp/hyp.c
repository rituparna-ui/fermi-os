#include "hyp.h"
#include "exception.h" /* ESR_EC, EC_HVC_AARCH64, ESR_ISS_* */
#include "stage2.h"
#include "vcpu.h"
#include "vgic/vgic.h"
#include "gic/gic.h" /* gic_enable_irq, gic_ack_irq, gic_end_irq */
#include "mm/pmm/pmm.h"
#include "mm/mmu/mmu.h" /* PHYS_TO_VIRT */
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

  uart_printf("[HYP] entering FermiOS guest: entry=%x VTTBR_EL2=%x\n",
              v.pc, v.vttbr);
  uart_println("[HYP] ---- guest output follows ----");

  /* Run the guest, servicing GICD/GICR MMIO so its gic_init completes.
   * Bounded so a guest fault loop can't hang the host. */
  long max_exits = 100000;
  uint64_t mmio_count = 0;
  while (max_exits-- > 0) {
    vcpu_enter(&v);
    uint64_t ec = ESR_EC_OF(v.esr);

    /* Stage-2 data abort: emulate if it targets the vGIC MMIO windows. */
    if (v.exit_reason == HYP_EXC_SYNC && ec == 0x24) {
      uint64_t ipa = ((v.hpfar >> 4) << 12) | (v.far & 0xFFF);
      if (vgic_mmio_is_target(ipa)) {
        uint64_t iss = v.esr & 0x1FFFFFFULL;
        int isv = (int)((iss >> 24) & 1);
        if (!isv) {
          uart_printf("\n[HYP] vGIC MMIO with ISV=0 at IPA=%x — cannot decode\n",
                      ipa);
          break;
        }
        int is_write = (int)((iss >> 6) & 1);
        int sas = (int)((iss >> 22) & 3);
        int srt = (int)((iss >> 16) & 0x1F);
        int sf = (int)((iss >> 15) & 1);
        int size_bytes = 1 << sas;

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
        v.pc += 4; /* advance past the trapped load/store */
        mmio_count++;
        continue;
      }
      /* A real stage-2 fault elsewhere — report and stop. */
      uart_printf("\n[HYP] guest stage-2 data abort OUTSIDE vGIC: IPA=%x pc=%x ESR=%x\n",
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
