#ifndef HYP_HVC_H
#define HYP_HVC_H

#include <stdint.h>
#include "../vcpu.h"

/* ---------------------------------------------------------------------------
 * Unified HVC hypercall ABI for FermiOS guests.
 *
 * A guest issues `hvc #0` with the function ID in x0 and arguments in x1..x7;
 * the return value (for informational calls) comes back in x0. This single
 * dispatcher folds in PSCI (power management) and a small set of FermiOS
 * para-virt "vendor" services. Calls that change run state (reset/off) or need
 * the scheduler (yield, doorbell) cannot complete inside the dispatcher, so
 * hvc_dispatch() returns an action the vCPU run-loop carries out.
 *
 * Function IDs follow the SMC Calling Convention number space:
 *   - PSCI:   0x8400_00xx (32-bit) / 0xC400_00xx (64-bit)   [see psci.h]
 *   - Vendor: 0xC600_00xx (64-bit owner=Vendor Hyp range)
 * ------------------------------------------------------------------------- */

/* FermiOS vendor hypercalls. */
#define HVC_VENDOR_BASE   0xC6000000ULL
#define HVC_FN_VERSION    (HVC_VENDOR_BASE + 0x00) /* -> x0 = ABI version       */
#define HVC_FN_PUTC       (HVC_VENDOR_BASE + 0x01) /* x1 = char -> host console */
#define HVC_FN_VM_INFO    (HVC_VENDOR_BASE + 0x02) /* -> x0=vmid, x1=vm_count   */
#define HVC_FN_YIELD      (HVC_VENDOR_BASE + 0x03) /* cooperatively end slice    */
#define HVC_FN_DOORBELL   (HVC_VENDOR_BASE + 0x04) /* signal the peer VM (SPI)   */

#define HVC_ABI_VERSION   0x00010000ULL            /* v1.0 */
#define HVC_RET_NOT_SUPP  ((uint64_t)-1)

/* Action the run-loop must take after hvc_dispatch(). */
typedef enum {
  HVC_ACT_NONE = 0, /* handled in place (x0 set); resume the guest        */
  HVC_ACT_YIELD,    /* cooperatively end this guest's slice               */
  HVC_ACT_RESET,    /* warm-reset this guest (PSCI SYSTEM_RESET)          */
  HVC_ACT_OFF,      /* power off this guest (PSCI SYSTEM_OFF / CPU_OFF)   */
  HVC_ACT_DOORBELL, /* ring the peer VM's doorbell                        */
} hvc_action_t;

/* Hypercall context the dispatcher may need (the running vCPU + its id). The
 * run-loop supplies these; PUTC needs nothing else, DOORBELL/VM_INFO use them. */
typedef struct {
  uint32_t vm_count; /* number of VMs in this run (for VM_INFO)            */
} hvc_env_t;

/* Decode the HVC in v->x[0..7]. Writes the return value(s) into v->x[] for
 * informational calls; returns the action for the rest. */
hvc_action_t hvc_dispatch(vcpu_t *v, const hvc_env_t *env);

#endif /* HYP_HVC_H */
