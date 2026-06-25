#include "snapshot.h"
#include "vm.h"      /* VMCTL_OK / VMCTL_EINVAL */
#include "hyp.h"
#include "hyp_alloc.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * One boot-reserved snapshot slot. The hyp bump allocator has no free(), so we
 * NEVER allocate per-hypercall — a single fixed slot is carved once at boot.
 * ------------------------------------------------------------------------- */
typedef struct {
  int      valid;       /* 0 until a successful save                        */
  int      id;          /* target vcpu id this snapshot belongs to          */
  uint32_t vmid;        /* target VMID, for the restore identity check      */
  uint64_t ram_size;    /* target RAM window length, for the identity check */

  /* Captured execution state (copied from the target's vcpu_t — the target
   * is never the current vCPU, so these struct fields are authoritative). */
  vcpu_gp_t      gp;
  vcpu_sysregs_t sys;
  vcpu_fp_t      fp;
  vcpu_vgic_t    vgic;

  /* vtimer captured time-RELATIVE (cval is an absolute CNTPCT deadline). */
  uint64_t vt_delta;  /* ticks from snapshot-time to the deadline (0 if n/a) */
  uint64_t vt_ctl;
  int      vt_armed;  /* was the timer an armed future deadline at capture?  */

  uint64_t *ram;      /* host-PA buffer for the guest's private RAM          */
} snapshot_slot_t;

static snapshot_slot_t slot;

static uint64_t snap_cntpct(void) {
  uint64_t t;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(t));
  return t;
}

/* The freestanding hyp has no libc; GCC lowers struct assignments to memcpy,
 * so provide one. (Byte-wise; the structs are small + infrequent.) */
void *memcpy(void *dst, const void *src, unsigned long n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (unsigned long i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void snapshot_init(void) {
  /* Reserve the RAM buffer once (rounded up to whole pages). */
  uint64_t pages = (SNAP_MAX_RAM + HYP_PAGE_SIZE - 1) / HYP_PAGE_SIZE;
  slot.ram = (uint64_t *)hyp_alloc_pages(pages);
  slot.valid = 0;
  hyp_puts("[SNAP] reserved snapshot slot (");
  hyp_puthex(SNAP_MAX_RAM);
  hyp_puts(" bytes RAM)\n");
}

static void mem_copy64(uint64_t dst, uint64_t src, uint64_t len) {
  volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
  const volatile uint64_t *s = (const volatile uint64_t *)(uintptr_t)src;
  uint64_t n = len / 8;
  for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

int64_t snapshot_save(int id) {
  vcpu_t *t = vcpu_by_id(id);
  if (!t || t == cur_vcpu) {
    return VMCTL_EINVAL; /* must target another VM (never the live caller) */
  }
  if (t->ram_size > SNAP_MAX_RAM) {
    return VMCTL_EINVAL; /* too big for the slot (e.g. 8 GiB FermiOS) */
  }

  /* Capture execution state purely from the (non-current) target's struct. */
  slot.gp   = t->gp;
  slot.sys  = t->sys;
  slot.fp   = t->fp;
  slot.vgic = t->vgic;

  /* vtimer: store the deadline RELATIVE to now so a later restore re-bases it
   * instead of replaying a stale absolute deadline (which would storm). */
  uint64_t now = snap_cntpct();
  slot.vt_ctl = t->vtimer.ctl;
  if ((t->vtimer.ctl & 1ULL) /*ENABLE*/ && !t->vtimer.pending &&
      t->vtimer.cval > now) {
    slot.vt_armed = 1;
    slot.vt_delta = t->vtimer.cval - now;
  } else {
    slot.vt_armed = 0;
    slot.vt_delta = 0;
  }

  /* Capture-side coherence: the guest ran with caches ON; clean its RAM to PoC
   * so our EL2-MMU-off read observes the guest's latest writes (no-op on QEMU,
   * correct on real HW). Then copy the live RAM window by ram_size. */
  hyp_dcache_clean_range(t->img_dst_pa, t->ram_size);
  mem_copy64((uint64_t)(uintptr_t)slot.ram, t->img_dst_pa, t->ram_size);

  slot.id = id;
  slot.vmid = t->vmid;
  slot.ram_size = t->ram_size;
  slot.valid = 1;

  hyp_puts("[SNAP] saved VM '");
  hyp_puts(t->name);
  hyp_puts("' (");
  hyp_puthex(t->ram_size);
  hyp_puts(" bytes RAM + full vCPU state)\n");
  return VMCTL_OK;
}

/* Apply the snapshot slot's captured state onto vcpu `t`. Shared by restore
 * (t == the snapshot's origin VM) and clone/migrate (t is a DIFFERENT VM with
 * the same ram_size). t must NOT be the current vCPU and must not be dead;
 * t->ram_size must equal the snapshot's. The guest state is host-PA- and
 * VMID-agnostic (it references IPAs + virtual INTIDs), so it transplants into
 * t's own stage-2 (t->vttbr_el2 / t->img_dst_pa) unchanged. */
static void apply_snapshot_to(vcpu_t *t) {
  int was_paused = t->paused;
  t->runnable = 0; /* quiesce: scheduler must not pick t mid-apply */

  /* Restore RAM into t's OWN backing store, then make it coherent for data +
   * instruction fetch (the guest may have self-modified .text). */
  mem_copy64(t->img_dst_pa, (uint64_t)(uintptr_t)slot.ram, t->ram_size);
  hyp_dcache_clean_range(t->img_dst_pa, t->ram_size);
  __asm__ __volatile__("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

  /* Flush t's OWN VMID's stage-2+combined TLB (NOT the caller dom0's). */
  uint64_t saved_vttbr;
  __asm__ __volatile__("mrs %0, vttbr_el2" : "=r"(saved_vttbr));
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(t->vttbr_el2));
  __asm__ __volatile__("dsb ish\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb" ::: "memory");
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(saved_vttbr));

  /* Execution state (memory-only; loaded into HW on the next vcpu_load). */
  t->gp   = slot.gp;
  t->sys  = slot.sys;
  t->fp   = slot.fp;
  t->vgic = slot.vgic;

  /* Re-base the absolute vtimer deadline to now + captured delta. */
  uint64_t now = snap_cntpct();
  if (slot.vt_armed) {
    t->vtimer.ctl = slot.vt_ctl;
    t->vtimer.cval = now + slot.vt_delta;
    t->vtimer.pending = 0;
  } else {
    t->vtimer.ctl = slot.vt_ctl & ~1ULL; /* clear ENABLE */
    t->vtimer.cval = 0;
    t->vtimer.pending = 0;
  }

  /* Identity + accounting fields (vmid, vttbr, img_x, run_count, cpu_ticks,
   * stats, weight, doorbell_target, privileged) are left untouched — for clone
   * this is exactly what keeps the destination's own container identity. */

  t->dead = 0;
  t->paused = was_paused;
  if (!was_paused) {
    t->runnable = 1;
  }
  hyp_cnthp_arm();
}

int64_t snapshot_restore(int id) {
  vcpu_t *t = vcpu_by_id(id);
  if (!t || t == cur_vcpu) {
    return VMCTL_EINVAL;
  }
  /* Restore goes back onto the SAME VM the snapshot came from: require an exact
   * id+vmid+ram_size match so we never splat one VM's image onto another here. */
  if (!slot.valid || slot.id != id || slot.vmid != t->vmid ||
      slot.ram_size != t->ram_size) {
    return VMCTL_EINVAL;
  }
  if (t->dead) {
    return VMCTL_EINVAL; /* never resurrect a powered-off VM */
  }

  apply_snapshot_to(t);
  hyp_puts("[SNAP] restored VM '");
  hyp_puts(t->name);
  hyp_puts("' from snapshot\n");
  return VMCTL_OK;
}

int64_t snapshot_clone(int dst_id) {
  vcpu_t *t = vcpu_by_id(dst_id);
  if (!t || t == cur_vcpu) {
    return VMCTL_EINVAL;
  }
  /* Clone/migrate: transplant the snapshot into a DIFFERENT VM. We do NOT
   * require id/vmid to match (that is the whole point) — only that a valid
   * snapshot exists, the destination is large enough (same ram_size), and the
   * destination is not the snapshot's own origin (use RESTORE for that) and not
   * dead. The captured state is IPA/VMID-agnostic, so it runs in t's stage-2. */
  if (!slot.valid || slot.ram_size != t->ram_size) {
    return VMCTL_EINVAL;
  }
  if (slot.id == dst_id) {
    return VMCTL_EINVAL; /* same VM — that's RESTORE, not clone */
  }
  if (t->dead) {
    return VMCTL_EINVAL;
  }

  apply_snapshot_to(t);
  hyp_puts("[SNAP] cloned snapshot of vcpu ");
  hyp_puthex((uint64_t)slot.id);
  hyp_puts(" into VM '");
  hyp_puts(t->name);
  hyp_puts("' (live migration)\n");
  return VMCTL_OK;
}
