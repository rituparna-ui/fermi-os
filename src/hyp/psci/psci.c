#include "psci.h"

psci_action_t psci_handle(vcpu_t *v) {
  uint64_t fn = v->x[0];

  switch (fn) {
  case PSCI_VERSION_FN:
    v->x[0] = 0x00010001; /* PSCI v1.1 */
    return PSCI_ACT_NONE;

  case PSCI_FEATURES_FN: {
    uint64_t q = v->x[1];
    /* Report the state-changing calls we implement as available (0). */
    if (q == PSCI_SYSTEM_RESET_FN || q == PSCI_SYSTEM_OFF_FN ||
        q == PSCI_CPU_OFF_FN || q == PSCI_VERSION_FN) {
      v->x[0] = PSCI_RET_SUCCESS;
    } else {
      v->x[0] = (uint64_t)PSCI_RET_NOT_SUPPORTED;
    }
    return PSCI_ACT_NONE;
  }

  case PSCI_SYSTEM_RESET_FN:
    return PSCI_ACT_RESET;

  case PSCI_SYSTEM_OFF_FN:
  case PSCI_CPU_OFF_FN:
    return PSCI_ACT_OFF;

  default:
    v->x[0] = (uint64_t)PSCI_RET_NOT_SUPPORTED;
    return PSCI_ACT_NONE;
  }
}
