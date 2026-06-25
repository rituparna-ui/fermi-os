#ifndef HYP_H
#define HYP_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * FermiOS type-1 hypervisor (EL2) — public interface.
 *
 * The hypervisor is a separate tiny image from the guest. It is loaded by
 * QEMU '-kernel' and entered at EL2 (CurrentEL==2). It links/runs at host
 * physical 0x250000000 — inside QEMU's RAM (-m 9G) but ABOVE the 8 GiB the
 * guest FermiOS manages (0x40000000..0x240000000), so the two never collide
 * even before stage-2 is enabled.
 *
 * hyp_main() is the C entry called from hyp_boot.S after the low-level EL2
 * register setup. It places the guest image and returns; hyp_boot.S then
 * performs the eret down to the guest at EL1.
 * ------------------------------------------------------------------------- */

/* Host physical base the hyp image is linked at (must match linker_hyp.ld and
 * the QEMU '-m' size leaving this region above guest RAM). */
#define HYP_PHYS_BASE 0x250000000ULL

/* Called from hyp_boot.S at EL2 with the MMU off. Prepares the guest for the
 * eret (Milestone 1: just announces and validates; the guest bytes are placed
 * at IPA/PA 0x40000000 by QEMU '-device loader'). */
void hyp_main(void);

/* Minimal EL2 console (direct PL011 MMIO at physical 0x09000000). Safe to call
 * with the MMU off; does not depend on any guest state. */
void hyp_putc(char c);
void hyp_puts(const char *s);
void hyp_puthex(uint64_t v);

/* Fatal EL2 error: print a message + spin. Reached from the EL2 vector stubs
 * and on impossible boot conditions (e.g. not actually at EL2). */
__attribute__((noreturn)) void hyp_panic(const char *msg);

#endif /* HYP_H */
