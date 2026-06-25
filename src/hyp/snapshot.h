#ifndef HYP_SNAPSHOT_H
#define HYP_SNAPSHOT_H

#include <stdint.h>
#include "vcpu.h"

/* ---------------------------------------------------------------------------
 * VM snapshot / restore (checkpoint + rollback).
 *
 * Captures a guest's complete execution + memory state to a hypervisor-owned
 * buffer, and rolls it back later. Foundation of checkpointing / live
 * migration. Design hardened against an adversarial review — key hazards:
 *   - vtimer.cval is an ABSOLUTE CNTPCT deadline: stored RELATIVE, rebased on
 *     restore (else a stale deadline storms the timer IRQ).
 *   - the snapshot buffer is ONE boot-reserved fixed slot (the hyp bump
 *     allocator has no free()); ram_size is capped, so 8 GiB FermiOS is
 *     deliberately not snapshottable.
 *   - restore flushes the TARGET VMID's stage-2 TLB (not the caller's), and
 *     makes the rewritten RAM I-cache coherent.
 *   - identity + accounting fields (vmid, vttbr, img_x, run_count, cpu_ticks,
 *     stats) are NEVER rolled back; a dead VM is never resurrected, a paused VM
 *     never silently un-paused.
 * Capture reads from the target's vcpu_t (it is NOT the current vCPU — dom0 is),
 * so no hardware reads are involved.
 * ------------------------------------------------------------------------- */

/* Hard cap on snapshottable guest RAM. Covers the 64 MiB / 16 MiB guests;
 * excludes the 8 GiB FermiOS by design (can't fit the ~765 MiB hyp pool). */
#define SNAP_MAX_RAM (64ULL * 1024 * 1024)

/* Reserve the snapshot slot once at boot (RAM buffer + metadata). */
void snapshot_init(void);

/* Capture vcpu `id` into the snapshot slot. Returns VMCTL_OK or a VMCTL error.
 * Rejects: non-existent id, the caller itself, ram_size > SNAP_MAX_RAM. */
int64_t snapshot_save(int id);

/* Restore vcpu `id` from the snapshot slot. Returns VMCTL_OK or a VMCTL error.
 * Rejects: no valid snapshot, id/vmid/ram_size mismatch, a dead target. */
int64_t snapshot_restore(int id);

#endif /* HYP_SNAPSHOT_H */
