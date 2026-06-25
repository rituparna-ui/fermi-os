#include "hvc.h"
#include "../psci/psci.h"
#include "uart/uart.h"

/* Unified HVC dispatcher: PSCI (folded in) + FermiOS vendor services. */

hvc_action_t hvc_dispatch(vcpu_t *v, const hvc_env_t *env) {
  uint64_t fn = v->x[0];

  /* --- PSCI range: delegate to the PSCI provider, map its action. --- */
  if ((fn & 0xFFFFFF00ULL) == 0x84000000ULL ||
      (fn & 0xFFFFFF00ULL) == 0xC4000000ULL) {
    switch (psci_handle(v)) {
    case PSCI_ACT_RESET: return HVC_ACT_RESET;
    case PSCI_ACT_OFF:   return HVC_ACT_OFF;
    case PSCI_ACT_NONE:
    default:             return HVC_ACT_NONE;
    }
  }

  /* --- FermiOS vendor range. --- */
  switch (fn) {
  case HVC_FN_VERSION:
    v->x[0] = HVC_ABI_VERSION;
    return HVC_ACT_NONE;

  case HVC_FN_PUTC:
    /* Emit one character to the host console, tagged with the VM name. We use
     * the guest's own vuart so output stays attributed and line-buffered. */
    vuart_emulate(&v->vuart, 0x09000000ULL /*UART DR*/, /*is_write=*/1,
                  &v->x[1], /*size=*/1);
    v->x[0] = 0;
    return HVC_ACT_NONE;

  case HVC_FN_VM_INFO:
    v->x[0] = v->vmid;
    v->x[1] = env ? env->vm_count : 1;
    return HVC_ACT_NONE;

  case HVC_FN_YIELD:
    v->x[0] = 0;
    return HVC_ACT_YIELD;

  case HVC_FN_DOORBELL:
    v->x[0] = 0;
    return HVC_ACT_DOORBELL;

  default:
    uart_printf("[HVC] %s: unknown hypercall fn=%x\n",
                v->name ? v->name : "vm", fn);
    v->x[0] = HVC_RET_NOT_SUPP;
    return HVC_ACT_NONE;
  }
}
