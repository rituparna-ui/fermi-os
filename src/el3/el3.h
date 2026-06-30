#ifndef EL3_EL3_H
#define EL3_EL3_H

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

#endif /* EL3_EL3_H */
