#include "vm.h"
#include "hyp.h"
#include "hyp_gic.h"
#include "timer/vtimer.h"
#include "vgic/vgic.h"
#include "virtio/virtio_rng.h"
#include "virtio/virtio_blk.h"
#include "virtio/virtio_net.h"
#include "vcpu.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EL2 trap-and-emulate dispatcher. Entered from hyp_vectors.S with the guest
 * GPRs + EL2 syndrome saved in `f`. Mutating f->regs[]/f->elr changes what the
 * guest sees on eret.
 * ------------------------------------------------------------------------- */

static const char *ec_name(uint64_t ec) {
  switch (ec) {
  case EC_WF_TRAPPED:    return "WFx";
  case EC_TRAPPED_SYSREG:return "sysreg";
  case EC_HVC_AARCH64:   return "HVC";
  case EC_SMC_AARCH64:   return "SMC";
  case EC_INST_ABORT_LO: return "inst-abort";
  case EC_DATA_ABORT_LO: return "data-abort";
  default:               return "?";
  }
}

/* An unhandled trap FROM A GUEST (lower EL). Report it, then fault-isolate the
 * offending VM (reboot just it, or power it off past its fault budget) and let
 * the rest of the machine keep running — instead of panicking the whole box.
 * Returns after rewriting the live frame; the vector exit erets into the
 * rebooted guest or the next VM. (Traps from EL2 itself are real hypervisor
 * bugs and still panic — see the current-EL vector stubs / handle_irq.) */
static void hyp_fatal_trap(uint64_t type, hyp_trap_frame_t *f) {
  uint64_t ec = ESR_EC(f->esr);
  hyp_puts("\n[HYP][TRAP] guest '");
  hyp_puts(cur_vcpu->name);
  hyp_puts("' type=");
  hyp_puthex(type);
  hyp_puts(" EC=");
  hyp_puthex(ec);
  hyp_puts(" (");
  hyp_puts(ec_name(ec));
  hyp_puts(")\n  ESR_EL2=");
  hyp_puthex(f->esr);
  hyp_puts(" ELR_EL2=");
  hyp_puthex(f->elr);
  hyp_puts("\n  FAR_EL2=");
  hyp_puthex(f->far);
  hyp_puts(" HPFAR_EL2=");
  hyp_puthex(f->hpfar);
  hyp_putc('\n');
  vcpu_fault_isolate(f); /* reboot/kill just this VM; machine stays up */
}

/* Advance ELR_EL2 past the trapped (4-byte AArch64) instruction. Used for
 * data/instruction aborts, trapped sysregs, and WFx — NOT for HVC/SMC (their
 * ELR already points past the instruction). */
static void advance_elr(hyp_trap_frame_t *f) { f->elr += 4; }

static void handle_psci(hyp_trap_frame_t *f) {
  uint64_t fn = f->regs[0];
  switch (fn) {
  case PSCI_VERSION_FN:
    f->regs[0] = 0x00010001; /* PSCI v1.1 */
    break;
  case PSCI_SYSTEM_OFF_FN:
    /* Power off this VM: mark it dead and switch away. If it was the last
     * runnable VM, the scheduler keeps it (nothing else to run) — for a
     * 2-VM demo SYSTEM_OFF from one VM lets the other keep running. */
    hyp_puts("[HYP] VM '");
    hyp_puts(cur_vcpu->name);
    hyp_puts("' requested PSCI SYSTEM_OFF\n");
    vcpu_poweroff_current(f);
    break;
  case PSCI_SYSTEM_RESET_FN:
    /* Warm-reset THIS VM: reload its pristine image and restart it from its
     * entry point. The other VM is unaffected — a per-VM reset, not a machine
     * reset. */
    vcpu_reset(cur_vcpu, f);
    break;
  case PSCI_FEATURES_FN:
  case PSCI_CPU_ON_FN64:
  case PSCI_AFFINITY_INFO_FN64:
  default:
    f->regs[0] = (uint64_t)PSCI_NOT_SUPPORTED;
    break;
  }
}

/* Reconstruct the faulting guest IPA from HPFAR_EL2 (IPA[47:12]) + FAR_EL2
 * (page offset[11:0]). */
static uint64_t fault_ipa(hyp_trap_frame_t *f) {
  uint64_t ipa_hi = (f->hpfar >> 4) << 12; /* HPFAR[39:4] -> IPA[47:12] */
  return ipa_hi | (f->far & 0xFFF);
}

static void handle_data_abort(uint64_t type, hyp_trap_frame_t *f) {
  uint64_t ipa = fault_ipa(f);

  /* Route to whichever emulated MMIO device owns this IPA window. */
  int is_vgic = vgic_mmio_is_target(ipa);
  int is_virtio = virtio_mmio_is_target(ipa);
  int is_vblk = virtio_blk_mmio_is_target(ipa);
  int is_vnet = virtio_net_mmio_is_target(ipa);
  if (!is_vgic && !is_virtio && !is_vblk && !is_vnet) {
    hyp_puts("\n[HYP] stage-2 data abort outside emulated MMIO, IPA=");
    hyp_puthex(ipa);
    hyp_putc('\n');
    hyp_fatal_trap(type, f);
    return; /* fault-isolated: f now belongs to the rebooted/next VM — must NOT
            * fall through and keep emulating against it. */
  }

  uint64_t esr = f->esr;
  if (!ISS_ISV(esr)) {
    /* No decoded syndrome — would require software instruction decode at
     * ELR_EL2. The guests use plain 32-bit str/ldr which QEMU decodes, so we
     * expect ISV=1; surface loudly if not. */
    hyp_puts("\n[HYP] MMIO abort with ISV=0 (need insn decode), IPA=");
    hyp_puthex(ipa);
    hyp_putc('\n');
    hyp_fatal_trap(type, f);
    return;
  }

  int is_write = (int)ISS_WNR(esr);
  uint32_t srt = ISS_SRT(esr);
  int sas = (int)ISS_SAS(esr);
  int size_bytes = 1 << sas;

  uint64_t val = 0;
  if (is_write) {
    /* xzr (reg 31) reads as 0. */
    val = (srt == 31) ? 0 : f->regs[srt];
    if (is_vnet)        virtio_net_mmio_emulate(ipa, 1, &val, size_bytes);
    else if (is_vblk)   virtio_blk_mmio_emulate(ipa, 1, &val, size_bytes);
    else if (is_virtio) virtio_mmio_emulate(ipa, 1, &val, size_bytes);
    else                vgic_mmio_emulate(ipa, 1, &val, size_bytes);
  } else {
    if (is_vnet)        virtio_net_mmio_emulate(ipa, 0, &val, size_bytes);
    else if (is_vblk)   virtio_blk_mmio_emulate(ipa, 0, &val, size_bytes);
    else if (is_virtio) virtio_mmio_emulate(ipa, 0, &val, size_bytes);
    else                vgic_mmio_emulate(ipa, 0, &val, size_bytes);
    if (srt != 31) {
      /* SF=0 (32-bit) zero-extends into the 64-bit reg, which writing the
       * masked value already achieves. */
      f->regs[srt] = ISS_SF(esr) ? val : (val & 0xFFFFFFFFULL);
    }
  }
  advance_elr(f);
}

static void handle_sync(uint64_t type, hyp_trap_frame_t *f) {
  uint64_t ec = ESR_EC(f->esr);
  switch (ec) {
  case EC_HVC_AARCH64:
    /* HVC: ELR already past the instruction — do NOT advance. */
    if (f->regs[0] == HVC_FERMI_YIELD) {
      /* Cooperative yield to another VM. */
      f->regs[0] = 0;
      vcpu_sched_tick(f);
    } else if (f->regs[0] == HVC_FERMI_DOORBELL) {
      /* Ring the doorbell: inject the doorbell vINTID into this VM's peer and
       * make it runnable. An event-channel-style notification — the peer takes
       * a virtual IRQ instead of polling. */
      f->regs[0] = (uint64_t)vcpu_ring_doorbell(cur_vcpu);
    } else if (f->regs[0] == HVC_FERMI_VMCTL) {
      /* Management hypercall from the privileged "dom0" control VM. */
      f->regs[0] = (uint64_t)vcpu_vmctl(f->regs[1], f->regs[2], f->regs[3], f);
    } else if (f->regs[0] == HVC_FERMI_LOG) {
      /* PV console: print the guest's buffer tagged with its VM name. */
      f->regs[0] = (uint64_t)vcpu_pv_log(f->regs[1], f->regs[2]);
    } else if (f->regs[0] == HVC_FERMI_WDOG) {
      /* Arm/pet the liveness watchdog (x1 = timeout ticks, 0 = disarm). */
      vcpu_wdog_arm(f->regs[1]);
      f->regs[0] = 0;
    } else {
      handle_psci(f);
    }
    break;

  case EC_SMC_AARCH64:
    /* SMC routes to PSCI too; like HVC, ELR already points past — no advance. */
    handle_psci(f);
    break;

  case EC_TRAPPED_SYSREG:
    if (vtimer_emulate_sysreg(f)) {
      advance_elr(f);
    } else {
      hyp_fatal_trap(type, f);
    }
    break;

  case EC_WF_TRAPPED:
    /* Guest WFI/WFE — the guest is idle. Skip the instruction so it re-checks
     * for a pending vIRQ on resume. The guest is idle, so BLOCK it and switch
     * to another runnable VM; it is woken precisely on its own vtimer deadline
     * by vcpu_wake_expired (its deadline is folded into CNTHP for all vCPUs).
     * This gives fair time-sharing: an idle guest yields the CPU to a busy one
     * instead of busy-trapping WFI for its whole slice. */
    advance_elr(f);
    vcpu_block_current(f);
    break;

  case EC_DATA_ABORT_LO:
    handle_data_abort(type, f);
    break;

  default:
    hyp_fatal_trap(type, f);
  }
}

static void handle_irq(hyp_trap_frame_t *f) {
  /* A physical IRQ was routed to EL2 (HCR_EL2.IMO=1). Ack it on the host
   * interface, dispatch, and EOI. Two PPIs are enabled at EL2:
   *   26 (CNTHP) — the running guest's virtualized EL1 physical timer
   *   28 (CNTHV) — the hypervisor's own round-robin scheduler tick */
  uint32_t intid = hyp_gic_ack();
  if (intid == 1023) {
    return; /* spurious */
  }
  if (intid == HYP_CNTHP_PPI) {
    /* CNTHP is the soonest of: the scheduler slice and EVERY vCPU's vtimer
     * deadline. First inject timer IRQs into (and wake) any vCPU whose deadline
     * elapsed — including blocked, non-current ones. */
    int must_switch = vcpu_wake_expired();

    uint64_t now;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

    if (now >= hyp_sched_deadline()) {
      /* Slice elapsed -> round-robin (also re-arms CNTHP). */
      vcpu_sched_tick(f);
    } else if (must_switch) {
      /* Current guest is blocked and another woke up -> switch to it. */
      vcpu_sched_tick(f);
    } else {
      /* Only a vtimer fired for the current guest; re-arm CNTHP. */
      hyp_cnthp_arm();
    }
  } else {
    hyp_puts("[HYP] EL2 IRQ intid=");
    hyp_puthex(intid);
    hyp_putc('\n');
  }
  hyp_gic_eoi(intid);
}

/* Account a trap to the CURRENT vCPU's stats BEFORE handling it — a handler may
 * world-switch (changing cur_vcpu), and we want to credit the VM that trapped. */
static void account_exit(uint64_t type, hyp_trap_frame_t *f) {
  vcpu_stats_t *s = &cur_vcpu->stats;
  if (type == HYP_EXC_IRQ) {
    s->irq++;
    return;
  }
  if (type != HYP_EXC_SYNC) {
    return;
  }
  switch (ESR_EC(f->esr)) {
  case EC_HVC_AARCH64:
  case EC_SMC_AARCH64:    s->hvc++; break;
  case EC_TRAPPED_SYSREG: s->sysreg++; break;
  case EC_WF_TRAPPED:     s->wfx++; break;
  case EC_DATA_ABORT_LO:
  case EC_INST_ABORT_LO:  s->data_abort++; break;
  default: break;
  }
}

void hyp_dispatch(uint64_t type, hyp_trap_frame_t *f) {
  account_exit(type, f);
  switch (type) {
  case HYP_EXC_SYNC:
    handle_sync(type, f);
    break;
  case HYP_EXC_IRQ:
    handle_irq(f);
    break;
  case HYP_EXC_FIQ:
  case HYP_EXC_SERROR:
  default:
    hyp_fatal_trap(type, f);
  }
}
