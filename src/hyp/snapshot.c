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

int64_t snapshot_restore(int id) {
  vcpu_t *t = vcpu_by_id(id);
  if (!t || t == cur_vcpu) {
    return VMCTL_EINVAL;
  }
  /* Hard precondition: a valid snapshot for THIS exact target must exist —
   * else we'd splat a zero/garbage buffer over a live guest's RAM. */
  if (!slot.valid || slot.id != id || slot.vmid != t->vmid ||
      slot.ram_size != t->ram_size) {
    return VMCTL_EINVAL;
  }
  if (t->dead) {
    return VMCTL_EINVAL; /* never resurrect a powered-off VM */
  }

  /* Quiesce the target so the scheduler can't select it mid-restore. (Single
   * CPU + IRQs masked in the EL2 HVC handler means no true concurrency, but
   * this is the correct discipline.) */
  int was_paused = t->paused;
  t->runnable = 0;

  /* Restore RAM first, then make it coherent for data + instruction fetch
   * (the guest may have self-modified .text). Same sequence as hyp_copy_image. */
  mem_copy64(t->img_dst_pa, (uint64_t)(uintptr_t)slot.ram, t->ram_size);
  hyp_dcache_clean_range(t->img_dst_pa, t->ram_size);
  __asm__ __volatile__("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

  /* Flush the TARGET VMID's stage-2+combined TLB. s2_tlb_flush_all() would
   * flush the CALLER's (dom0's) VMID — wrong. Temporarily point VTTBR_EL2 at
   * the target, flush its VMID, then restore the caller's VTTBR_EL2. */
  uint64_t saved_vttbr;
  __asm__ __volatile__("mrs %0, vttbr_el2" : "=r"(saved_vttbr));
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(t->vttbr_el2));
  __asm__ __volatile__("dsb ish\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb" ::: "memory");
  __asm__ __volatile__("msr vttbr_el2, %0\n\tisb" ::"r"(saved_vttbr));

  /* Restore execution state (memory-only; takes effect on the next vcpu_load
   * when the scheduler world-switches in). */
  t->gp   = slot.gp;
  t->sys  = slot.sys;
  t->fp   = slot.fp;
  t->vgic = slot.vgic;

  /* Re-base the vtimer deadline to now + captured delta. If it wasn't an armed
   * future deadline at capture, force it DISABLED so a stale cval=0 is never
   * treated as an already-expired live deadline (which would storm). */
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
   * stats, weight, doorbell_target, privileged) are left untouched. */

  /* Resume policy: do not resurrect dead, do not silently un-pause an admin
   * pause; otherwise make it runnable so it resumes from the restored PC. */
  t->dead = 0;
  t->paused = was_paused;
  if (!was_paused) {
    t->runnable = 1;
  }

  /* Fold the (possibly newly-near) target deadline into the shared CNTHP. */
  hyp_cnthp_arm();

  hyp_puts("[SNAP] restored VM '");
  hyp_puts(t->name);
  hyp_puts("' from snapshot\n");
  return VMCTL_OK;
}
