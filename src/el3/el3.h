#ifndef EL3_EL3_H
#define EL3_EL3_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * el3.h — EL3 Root-world Secure Monitor
 *
 * On a FEAT_RME machine (QEMU `virt,secure=on,virtualization=on` + `-cpu
 * max,x-rme=on`) the CPU resets into EL3, the most privileged level, running
 * in the Root world. el3_init() is called from boot.S while still at EL3 with
 * the MMU off; boot.S then ERETs down into the Non-secure world to run Fermi
 * as the host. Later milestones add the Granule Protection Table + GPC here
 * and a path that ERETs into the Realm world to launch the RMM at Realm-EL2.
 * --------------------------------------------------------------------------- */

void el3_init(void);

/* RMM <-> EL3 interface (SMC). The RMM stub at Realm-EL2 issues this FID to
 * tell the monitor it has finished booting; EL3 then enters the Non-secure
 * world. (Modeled on the real RMM-EL3 boot handshake.) */
#define RMM_BOOT_COMPLETE 0xC4000010ULL

/* Host/RMM -> EL3 granule delegation (E2): flip a page's GPT ownership (GPI)
 * between Non-secure and Realm. Only EL3 can edit the GPT. */
#define SMC_GRANULE_DELEGATE 0xC4000011ULL   /* (pa) NS  -> Realm */
#define SMC_GRANULE_UNDELEGATE 0xC4000012ULL /* (pa) Realm -> NS  */

/* Register frame saved by the EL3 vector stubs (x0..x30). */
typedef struct {
  uint64_t x[31];
} el3_frame_t;

/* C handler for synchronous exceptions taken to EL3 (notably SMC). */
void el3_sync_handler(el3_frame_t *f);

#endif /* EL3_EL3_H */
