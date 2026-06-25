#include "hyp.h"
#include "exception.h" /* ESR_EC, EC_HVC_AARCH64, ESR_ISS_* */
#include "stage2.h"
#include "vcpu.h"
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
