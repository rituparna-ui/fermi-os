#ifndef HYP_PSCI_H
#define HYP_PSCI_H

#include <stdint.h>
#include "../vcpu.h"

/* ---------------------------------------------------------------------------
 * Minimal PSCI 1.1 provider for guest power-management hypercalls (HVC).
 *
 * FermiOS's `reboot` issues `hvc` with x0 = PSCI SYSTEM_RESET (0x84000009).
 * We service the common 32/64-bit function IDs. Calls that change run state
 * (SYSTEM_OFF / SYSTEM_RESET / CPU_OFF) cannot complete inside the dispatcher
 * (they need the scheduler), so psci_handle() returns an action code and the
 * caller (the vCPU run-loop) carries it out.
 * ------------------------------------------------------------------------- */

/* PSCI function IDs (SMC32 0x84.. and SMC64 0xC4.. ranges). */
#define PSCI_VERSION_FN       0x84000000ULL
#define PSCI_CPU_OFF_FN       0x84000002ULL
#define PSCI_SYSTEM_OFF_FN    0x84000008ULL
#define PSCI_SYSTEM_RESET_FN  0x84000009ULL
#define PSCI_FEATURES_FN      0x8400000AULL
#define PSCI_CPU_ON_FN64      0xC4000003ULL
#define PSCI_AFFINITY_INFO64  0xC4000004ULL

/* PSCI return values. */
#define PSCI_RET_SUCCESS        0
#define PSCI_RET_NOT_SUPPORTED  (-1)

/* Action the run-loop must take after psci_handle(). */
typedef enum {
  PSCI_ACT_NONE = 0,   /* handled in place (x0 set); resume the guest */
  PSCI_ACT_RESET,      /* warm-reset this guest                       */
  PSCI_ACT_OFF,        /* power off this guest (stop scheduling it)   */
  PSCI_ACT_CPU_ON,     /* bring up a secondary vCPU (SMP)             */
} psci_action_t;

/* Decode the PSCI call in v->x[0..]. Writes the return value into v->x[0] for
 * informational calls and returns the action for state-changing calls. */
psci_action_t psci_handle(vcpu_t *v);

#endif /* HYP_PSCI_H */
