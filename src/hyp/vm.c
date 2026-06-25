#include "vm.h"
#include "hyp.h"
#include "hyp_gic.h"
#include "timer/vtimer.h"
#include "vgic/vgic.h"
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

__attribute__((noreturn)) static void hyp_fatal_trap(uint64_t type,
                                                     hyp_trap_frame_t *f) {
  uint64_t ec = ESR_EC(f->esr);
  hyp_puts("\n[HYP][TRAP] type=");
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
  hyp_panic("unhandled guest trap");
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
    hyp_panic("guest requested PSCI SYSTEM_OFF — halting");
  case PSCI_SYSTEM_RESET_FN:
    hyp_panic("guest requested PSCI SYSTEM_RESET — halting (warm reset TODO)");
  case PSCI_FEATURES_FN:
  case PSCI_CPU_ON_FN64:
  case PSCI_AFFINITY_INFO_FN64:
  default:
    f->regs[0] = (uint64_t)PSCI_NOT_SUPPORTED;
    break;
  }
}

static void handle_sync(uint64_t type, hyp_trap_frame_t *f) {
  uint64_t ec = ESR_EC(f->esr);
  switch (ec) {
  case EC_HVC_AARCH64:
    /* HVC: ELR already past the instruction — do NOT advance. */
    handle_psci(f);
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
    /* Guest WFI/WFE. Nothing to do — a pending vINTID (if any) is already in a
     * List Register and will be presented on eret. Just skip the instruction
     * so the guest re-checks rather than spinning on the same WFI. */
    advance_elr(f);
    break;

  default:
    hyp_fatal_trap(type, f);
  }
}

static void handle_irq(void) {
  /* A physical IRQ was routed to EL2 (HCR_EL2.IMO=1). Ack it on the host
   * interface, dispatch, and EOI. The only physical IRQ we enable at EL2 is
   * the EL2 physical-timer PPI (26). */
  uint32_t intid = hyp_gic_ack();
  if (intid == 1023) {
    return; /* spurious */
  }
  if (intid == HYP_CNTHP_PPI) {
    vtimer_handle_host_irq();
  }
  hyp_gic_eoi(intid);
}

void hyp_dispatch(uint64_t type, hyp_trap_frame_t *f) {
  switch (type) {
  case HYP_EXC_SYNC:
    handle_sync(type, f);
    break;
  case HYP_EXC_IRQ:
    handle_irq();
    break;
  case HYP_EXC_FIQ:
  case HYP_EXC_SERROR:
  default:
    hyp_fatal_trap(type, f);
  }
}
