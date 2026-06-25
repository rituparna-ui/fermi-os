#ifndef HYP_VPCI_H
#define HYP_VPCI_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal virtual PCI host bridge + one PCI endpoint, emulated at EL2 via
 * stage-2 traps on a small ECAM (Enhanced Configuration Access Mechanism)
 * window. A guest enumerates the bus the standard way — read Vendor/Device ID
 * per (bus,slot,func), then size + assign a BAR via the write-all-ones probe,
 * then enable the device in the Command register.
 *
 * ECAM address layout (standard, matches the guest's pci.c):
 *   addr = ECAM_BASE | (bus<<20) | (slot<<15) | (func<<12) | reg
 * We model a single 4 KiB window = bus 0, slot 0, func 0 (one endpoint). A read
 * of any other (bus,slot,func) returns 0xFFFFFFFF (no device), so a brute-force
 * scan finds exactly our one device and nothing else.
 *
 * The emulated endpoint is a "fermi demo" device (vendor 0x1234, device 0xBEEF)
 * with a single 64 KiB 32-bit memory BAR. BAR sizing works the classic way:
 * the guest writes 0xFFFFFFFF and reads back the size mask; the guest then
 * programs a real base address, which the device latches.
 * ------------------------------------------------------------------------- */

#define VPCI_ECAM_BASE 0x0A003000ULL /* 4 KiB ECAM (bus0/slot0/func0) */
#define VPCI_ECAM_SIZE 0x1000ULL

int  vpci_mmio_is_target(uint64_t ipa);
void vpci_mmio_emulate(uint64_t ipa, int is_write, uint64_t *val,
                       int size_bytes);

#endif /* HYP_VPCI_H */
