#include "syscall.h"
#include "mm/mmu/mmu.h" // USER_STACK_TOP
#include "sched/sched.h"
#include "net/net.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "vfs/vfs.h"
#include "pci/virtio/balloon/balloon.h"
#include "pci/virtio/rng/rng.h"

#include "mm/heap/heap.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "elf.h"
#include "linux_abi.h"

#include <stddef.h>
#include <stdint.h>

// AAPCS64 syscall convention:
//   x8       = syscall number
//   x0 - x7  = up to 7 arguments
//   x0       = return value (written back into trap frame)

// User-space layout is [0, USER_VA_TOP). Anything outside that range is
// either an unmapped TTBR0 region or — much worse — a kernel pointer the user
// is trying to trick the syscall path into dereferencing on its behalf.
//
// USER_VA_TOP (1 GiB) replaced USER_STACK_TOP (8 MiB) as this bound when
// mmap() landed. The stack top was only ever correct while the stack was the
// sole user mapping above text; mmap hands out addresses well past it, so
// validating against 8 MiB would have rejected every mmap'd buffer as an
// out-of-range pointer. The security property is preserved because the kernel
// itself lives at 0xFFFF_0000_4000_0000 — nothing below 1 GiB can alias a
// kernel pointer.
//
// Limitation: these checks only validate the *range*. A user pointer inside
// the range that points to an unmapped page will still fault the kernel
// when the underlying fd op touches it. Hardening that requires either a
// kernel page-fault handler with fixup tables (Linux's exception_table) or
// an explicit copy_from_user that walks the page table first. Out of scope
// for this fix — the kernel-pointer-injection hole is what we close here.

#define USER_PATH_MAX 4096

/*
 * errno plumbing
 *
 * POSIX error reporting is a two-part contract: the syscall returns -1, AND
 * stores a reason in the caller's `errno`. errno must be per-thread, or two
 * tasks would overwrite each other's error codes.
 *
 * This kernel implements that with the architecture's own mechanism rather
 * than a kernel-side lookup table: each task owns a TLS page at USER_TLS_BASE
 * and TPIDR_EL0 points at it. context_switch reloads TPIDR_EL0 on every switch,
 * so the register is always correct for whichever task is about to run —
 * including a task resuming from a syscall that blocked mid-way.
 *
 * The store goes through PHYS_TO_VIRT rather than the user VA on purpose: the
 * kernel may be servicing a task whose TTBR0 has already been swapped out
 * (possible when a reaped task's frame is still on a stack), and the TLS page
 * is a physical page we own, so the high-half mapping always reaches it.
 */
static inline int32_t *task_errno_slot(task_t *t) {
  if (!t || !t->tls_phys) {
    return 0;
  }
  return (int32_t *)((uintptr_t)PHYS_TO_VIRT(t->tls_phys) + TLS_ERRNO_OFFSET);
}

/* Record `err` as the calling task's errno and return -1, so every failing
 * return site reads `return fail(EINVAL);`. */
static inline int64_t fail(int err) {
  task_t *t = sched_current();
  int32_t *slot = task_errno_slot(t);
  if (slot) {
    *slot = err;
  }
  return -1;
}

/* Clear errno. POSIX requires a successful call to leave errno alone, so this
 * is called only where the kernel genuinely wants it reset (task start). */
static inline void clear_errno(task_t *t) {
  int32_t *slot = task_errno_slot(t);
  if (slot) {
    *slot = 0;
  }
}

/* Called from sched when a task is first entered, so a program never observes
 * a stale errno inherited from whatever ran before it. */
void syscall_reset_errno(task_t *t) { clear_errno(t); }

// Returns 1 if [ptr, ptr+len) lies entirely inside the user address range.
// Zero-length buffers are allowed regardless of pointer (matches POSIX).
static inline int user_buf_ok(uint64_t ptr, size_t len) {
  if (len == 0) {
    return 1;
  }
  // Overflow guard first
  if (ptr + len < ptr) {
    return 0;
  }
  if (ptr + len > USER_VA_TOP) {
    return 0;
  }
  return 1;
}

// Validate a NUL-terminated user string. Returns string length (excluding
// NUL) on success, or -1 if ptr is out of range or no NUL is found within
// USER_PATH_MAX bytes.
static inline int64_t user_str_ok(uint64_t ptr) {
  if (ptr >= USER_VA_TOP) {
    return -1;
  }
  uint64_t bound = USER_VA_TOP - ptr;
  if (bound > USER_PATH_MAX) {
    bound = USER_PATH_MAX;
  }
  const char *s = (const char *)ptr;
  for (uint64_t i = 0; i < bound; i++) {
    if (s[i] == '\0') {
      return (int64_t)i;
    }
  }
  return -1; // not NUL-terminated within bound
}

/* SYS_EXEC — replace the calling task's user image with a flat binary read
 * from the VFS at `path`. The new binary is loaded at USER_TEXT_BASE, runs
 * with x0–x30 = 0 and SP_EL0 = USER_STACK_TOP. On success this function
 * does NOT actually return to user space at the SVC site — we modify the
 * trap frame so the eret epilogue lands in the new program's _start.
 *
 * Memory model:
 *   - Code: PMM-allocated pages, RO + EL0-executable, mapped at
 *     USER_TEXT_BASE in a fresh user_l0. Tracked in task->exec_text_phys
 *     so sched_reap (and the next exec) can free them.
 *   - Stack: fresh PMM-allocated pages, RW + UXN, mapped at
 *     [USER_STACK_TOP - 16 KiB, USER_STACK_TOP).
 *   - Old user_l0, old user-stack pages, and any prior exec text are freed
 *     after the TTBR0 swap so we don't yank out from under ourselves.
 *
 * Returns -1 on any failure (open/read/alloc); on success there's no
 * meaningful return because the new image starts running.
 */
#define EXEC_MAX_BYTES (1U << 20) /* 1 MiB cap */
#define EXEC_MAX_ARGC 32          /* hard limit on argv count */
#define EXEC_ARG_BYTES_MAX 1024   /* total string-byte budget for argv */

/* Append one (type, value) pair to a Linux auxiliary vector and advance *idx. */
static void aux_put(uint64_t *base, int *idx, uint64_t type, uint64_t val) {
  base[(*idx) * 2]     = type;
  base[(*idx) * 2 + 1] = val;
  (*idx)++;
}

static int64_t sys_exec(uint64_t arg_path, uint64_t arg_argv,
                        trap_frame_t *frame) {
  if (user_str_ok(arg_path) < 0) {
    return -1;
  }
  const char *path = (const char *)arg_path;

  task_t *cur     = sched_current();
  fd_table_t *fds = cur->fds;
  if (!fds) {
    return -1;
  }

  /* Validate + capture argv while OLD TTBR0 is still active. argv may be
   * NULL (no-args exec), or a NULL-terminated array of NUL-terminated
   * strings. Snapshot to kernel scratch buffers so the post-swap stack
   * can be populated without re-reading user memory.
   *
   * Stack layout we'll build (high → low):
   *   [argv string blob]                 (top)
   *   [16-byte alignment pad]
   *   argv[argc] = NULL
   *   argv[argc-1] = (user VA of str)
   *   ...
   *   argv[0] = (user VA of str)         ← argv pointer (passed in x1)
   *   <SP_EL0 sits 16-byte-aligned below>
   * The program receives argc in x0, argv in x1, envp (NULL) in x2. */
  char    arg_kbuf[EXEC_ARG_BYTES_MAX];
  size_t  arg_offsets[EXEC_MAX_ARGC];
  int     argc      = 0;
  size_t  arg_bytes = 0;

  if (arg_argv != 0) {
    while (argc < EXEC_MAX_ARGC) {
      uint64_t slot_va = arg_argv + (uint64_t)argc * sizeof(uint64_t);
      if (!user_buf_ok(slot_va, sizeof(uint64_t))) {
        return -1;
      }
      const char *p = ((const char **)arg_argv)[argc];
      if (p == NULL) {
        break; /* NULL-terminator slot */
      }
      int64_t len = user_str_ok((uint64_t)p);
      if (len < 0) {
        return -1;
      }
      if (arg_bytes + (size_t)len + 1 > EXEC_ARG_BYTES_MAX) {
        return -1;
      }
      arg_offsets[argc] = arg_bytes;
      memcpy(arg_kbuf + arg_bytes, p, (size_t)len + 1);
      arg_bytes += (size_t)len + 1;
      argc++;
    }
  }


  /* ---- Initial stack layout ------------------------------------------
   *
   * Computed here, before the address space has been swapped or a single page
   * freed, so an argv that cannot fit is refused with a clean return instead
   * of after the task is half-exec'd.
   *
   * The user stack is only 16 KiB (USER_STACK_PAGES=4) and this layout grows
   * with the argument strings, so the bound is checked rather than assumed.
   *
   * Low to high, from the address SP_EL0 will hold:
   *
   *   [sp]                    argc
   *   [sp + 8 ...]            argv[0..argc-1], then NULL
   *   [envp]                  a lone NULL -- this kernel passes no environment
   *   [envp + 8 ...]          auxiliary vector, terminated by an AT_NULL pair
   *   <16-byte aligned>
   *   argv strings, then 16 bytes of randomness for AT_RANDOM
   */
  /* AUX_MAX is the WORST CASE, not the actual count: AT_EXECFN is only emitted
   * when argv is non-empty, so a no-args program writes one fewer pair.
   * Reserving the maximum keeps the layout fixed and the bounds check above
   * valid for either case. */
  const uint64_t aux_max         = 17;
  const uint64_t aux_random_size = 16; /* AT_RANDOM always points at 16 bytes */

  uint64_t stack_user_lo = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  uint64_t envp_off      = 8 + (uint64_t)(argc + 1) * sizeof(uint64_t);
  uint64_t auxv_off      = envp_off + sizeof(uint64_t);
  uint64_t auxv_base     = (auxv_off + aux_max * 16 + 15) & ~(uint64_t)15;
  uint64_t strings_lo    = (USER_STACK_TOP - arg_bytes - aux_random_size) &
                           ~(uint64_t)15;
  uint64_t new_sp        = strings_lo - auxv_base;

  if (new_sp < stack_user_lo) {
    uart_errorln("[EXEC] argv + auxiliary vector do not fit the user stack");
    return -1;
  }

  /* 1. Open and size the binary. */
  int fd = fd_open(fds, path);
  if (fd < 0) {
    uart_printf("[EXEC] open failed: %s\n", path);
    return -1;
  }
  int64_t size = fd_seek(fds, fd, 0, SEEK_END);
  fd_seek(fds, fd, 0, SEEK_SET);
  if (size <= 0 || (uint64_t)size > EXEC_MAX_BYTES) {
    uart_printf("[EXEC] bad size %ld for %s (max %u)\n", size, path,
                EXEC_MAX_BYTES);
    fd_close(fds, fd);
    return -1;
  }

  /* 2. Slurp into a kernel buffer. */
  uint8_t *kbuf = (uint8_t *)kmalloc((size_t)size);
  if (!kbuf) {
    uart_printf("[EXEC] kmalloc(%ld) failed for %s\n", size, path);
    fd_close(fds, fd);
    return -1;
  }
  int64_t got = fd_read(fds, fd, kbuf, (size_t)size);
  fd_close(fds, fd);
  if (got != size) {
    uart_printf("[EXEC] short read %ld != %ld for %s\n", got, size, path);
    kfree(kbuf);
    return -1;
  }

  /* 3. Fresh user stack pages — always 16 KiB, RW + UXN, mapped at the
   * top of the user range. The stack is independent of the binary; the
   * ELF loader handles only PT_LOAD segments. */
  uintptr_t stack_phys = pmm_allocate_pages(USER_STACK_PAGES);
  if (!stack_phys) {
    uart_errorln("[EXEC] no PMM for user stack");
    kfree(kbuf);
    return -1;
  }
  memset((void *)PHYS_TO_VIRT(stack_phys), 0, USER_STACK_PAGES * PAGE_SIZE);

  /* 4. Build a fresh user_l0 and load each PT_LOAD into it. elf_load
   * allocates per-segment PMM pages, copies file bytes, zeros .bss, and
   * maps with permissions derived from each segment's p_flags (RO+X for
   * .text, RW+UXN for .data + .bss, RO+UXN for .rodata). On failure it
   * frees any partial allocations before returning. */
  uint64_t *new_l0 = mmu_create_user_tables();
  if (!new_l0) {
    pmm_free_pages(stack_phys, USER_STACK_PAGES);
    kfree(kbuf);
    return -1;
  }

  elf_image_t img;
  if (elf_load(kbuf, (size_t)size, new_l0, &img) < 0) {
    /* elf_load already freed any per-segment pages it allocated; we still
     * need to tear down the user_l0 page tables and the stack. */
    mmu_free_user_tables(new_l0);
    pmm_free_pages(stack_phys, USER_STACK_PAGES);
    kfree(kbuf);
    return -1;
  }
  kfree(kbuf);

  /* 5. Map the user stack into the new user_l0. */
  uint64_t stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  uint64_t stack_user_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  mmu_map_user_range(new_l0, stack_user_base, stack_phys, USER_STACK_PAGES,
                     stack_flags);

  /* 5b. Give the new image its own TLS page.
   *
   * Without this the exec'd program inherits a TPIDR_EL0 pointing into the
   * OLD address space, which we are about to tear down. Every read and write
   * of errno would then hit unmapped memory — and since the faulting address
   * is whatever we wrote errno to, it can land at a wild offset rather than
   * faulting cleanly at 0. A fresh page is the only correct option: errno is
   * per-process, and a new image is a new process. */
  uintptr_t new_tls = pmm_allocate_page();
  if (!new_tls) {
    uart_errorln("[EXEC] Failed to allocate TLS page for new image");
    mmu_free_user_tables(new_l0);
    pmm_free_pages(stack_phys, USER_STACK_PAGES);
    for (int i = 0; i < img.region_count; i++) {
      if (img.regions[i].phys && img.regions[i].pages) {
        pmm_free_pages(img.regions[i].phys, img.regions[i].pages);
      }
    }
    return -1; /* kbuf was already freed above */
  }
  memset((void *)PHYS_TO_VIRT(new_tls), 0, PAGE_SIZE);
  mmu_map_user_range(new_l0, USER_TLS_BASE, new_tls, 1, stack_flags);

  /* Print diagnostic *before* the TTBR0 swap. `path` is a user pointer
   * into the OLD address space, which gets unmapped + freed below. */
  uart_printf("[EXEC] Task %d '%s' loading %s (%d bytes, %d region(s), entry %x)\n",
              cur->pid, cur->name, path, (uint64_t)size,
              (uint64_t)img.region_count, img.entry);


  /* 6. Swap in the new image. Save old refs first — we'll free them after
   * TTBR0 has been switched over so we don't yank our own mappings while
   * still using them. */
  uint64_t    old_ttbr0       = cur->ttbr0;
  uintptr_t   old_ustack_phys = cur->ustack_phys;
  uintptr_t   old_tls_phys    = cur->tls_phys;
  elf_image_t old_image       = cur->exec_image;

  /* Allocate a fresh ASID for the new VA layout. With ASIDs every TLB
   * entry is tagged: the new ASID has zero entries cached, so we can
   * safely skip the global tlbi vmalle1 — there's nothing to flush.
   * Stale entries from the old ASID remain in the TLB and will be
   * invalidated explicitly below before the old page tables are freed. */
  uint16_t new_asid  = sched_asid_alloc();
  uint64_t new_ttbr0 = ttbr_pack((uint64_t)new_l0, new_asid);

  cur->ttbr0       = new_ttbr0;
  cur->ustack_phys = stack_phys;
  cur->tls_phys    = new_tls;
  cur->tpidr_el0   = USER_TLS_BASE;
  cur->user_sp     = USER_STACK_TOP;
  cur->exec_image  = img;

  /* exec replaces the program image but keeps the process, so POSIX says the
   * address space is wiped and the break resets. VMAs (mmap regions) and the
   * brk arena belonged to the old image, so drop them here rather than
   * letting the new program inherit a heap full of the old one's data. */
  vma_destroy(&cur->vmas);
  vma_init(&cur->vmas);
  cur->brk_base = USER_BRK_BASE;
  cur->brk_cur  = USER_BRK_BASE;
  cur->stack_grown_count = 0;

  __asm__ __volatile__("msr ttbr0_el1, %0\n\t"
                       "isb"
                       ::"r"(new_ttbr0)
                       : "memory");

  /* 7. Rewrite the trap frame so the syscall epilogue ererts into the new
   * program's _start with a clean register state and the new SP_EL0. */
  for (int i = 0; i < 31; i++) {
    frame->regs[i] = 0;
  }
  frame->elr  = img.entry;
  frame->spsr = 0; /* EL0t, IRQs unmasked */

  /* sp_el0 lives at offset 280 in the 688-byte on-stack trap frame; the
   * C trap_frame_t (sizeof = 280) doesn't expose it. Poke directly. */
  uint64_t *frame_raw = (uint64_t *)frame;
  uintptr_t stack_kbase = PHYS_TO_VIRT(stack_phys);

  /* Copy the argv strings and the AT_RANDOM bytes to the top of the stack,
   * then build the pointer arrays and aux vector below them. Everything is
   * written through the kernel's alias of the fresh stack pages, so this does
   * not depend on TTBR0 already pointing at the new address space. */
  memcpy((void *)(stack_kbase + (strings_lo - stack_user_lo)), arg_kbuf,
         arg_bytes);

  uint64_t random_va = strings_lo + arg_bytes;
  memset((void *)(stack_kbase + (random_va - stack_user_lo)), 0,
         aux_random_size);
  /* Best-effort: AT_RANDOM must point at 16 readable bytes, but virtio-rng may
   * be absent. Falling back to zeros is harmless -- the value is only ever used
   * for stack-protector canaries and hash seeding, never as a secret. */
  rng_read((void *)(stack_kbase + (random_va - stack_user_lo)),
           (uint32_t)aux_random_size);

  uint64_t *sp_k = (uint64_t *)(stack_kbase + (new_sp - stack_user_lo));
  sp_k[0] = (uint64_t)argc;

  uint64_t *argv_k = &sp_k[1];
  for (int i = 0; i < argc; i++) {
    argv_k[i] = strings_lo + arg_offsets[i];
  }
  argv_k[argc] = 0; /* argv terminator */

  /* envp is a one-element array holding just NULL: no environment is passed. */
  uint64_t *envp_k = (uint64_t *)(stack_kbase + (new_sp + envp_off - stack_user_lo));
  envp_k[0] = 0;

  uint64_t *aux_k = (uint64_t *)(stack_kbase + (new_sp + auxv_off - stack_user_lo));
  int ai = 0;
  aux_put(aux_k, &ai, AT_PHDR,   img.phdr_va);
  aux_put(aux_k, &ai, AT_PHENT,  img.phentsize);
  aux_put(aux_k, &ai, AT_PHNUM,  img.phnum);
  aux_put(aux_k, &ai, AT_PAGESZ, PAGE_SIZE);
  /* AT_BASE is the dynamic loader's base. We load ET_EXEC only and have no
   * ld.so, so 0 correctly says "there is no interpreter". */
  aux_put(aux_k, &ai, AT_BASE,   0);
  aux_put(aux_k, &ai, AT_FLAGS,  0);
  aux_put(aux_k, &ai, AT_ENTRY,  img.entry);
  /* No user database exists, so every id is 0. Advertising 0 rather than
   * omitting them keeps a guest's getuid()/getgid() working. */
  aux_put(aux_k, &ai, AT_UID,    0);
  aux_put(aux_k, &ai, AT_EUID,   0);
  aux_put(aux_k, &ai, AT_GID,    0);
  aux_put(aux_k, &ai, AT_EGID,   0);
  /* AT_HWCAP = 0: this CPU is a fixed model with no runtime-detectable
   * feature bits, so a guest that trusts a non-zero value would assume
   * instructions we have not verified. */
  aux_put(aux_k, &ai, AT_HWCAP,  0);
  aux_put(aux_k, &ai, AT_CLKTCK, 100);
  aux_put(aux_k, &ai, AT_SECURE, 0);   /* not setuid/setgid */
  aux_put(aux_k, &ai, AT_RANDOM, random_va);
  if (argc > 0) {
    aux_put(aux_k, &ai, AT_EXECFN, strings_lo); /* argv[0], the path as given */
  }
  aux_put(aux_k, &ai, AT_NULL, 0);

  /* The AT_NULL pair is what terminates the walk -- a guest stops there, so the
   * count itself is not part of the contract. Zero any slots the conditional
   * AT_EXECFN left unused so the whole area is deterministic rather than
   * whatever was on the stack page. */
  for (uint64_t i = (uint64_t)ai; i < aux_max; i++) {
    aux_k[i * 2]     = 0;
    aux_k[i * 2 + 1] = 0;
  }

  /* Registers: keep the AAPCS64 convention our own libc's crt0 uses while
   * ALSO handing the program the Linux stack above. Both entry contracts are
   * satisfied at once, so Fermi-linked and foreign binaries need no
   * negotiation to tell them apart. */
  frame->regs[0] = (uint64_t)argc;
  frame->regs[1] = new_sp + sizeof(uint64_t); /* &argv[0] */
  frame->regs[2] = new_sp + envp_off;        /* &envp[0] */

  frame_raw[35] = new_sp;



  /* 8. Free old image now that we're no longer using it. mmu_free_user_tables
   * walks intermediate tables only — leaf data pages (text, stack) are
   * freed explicitly above. */
  if (old_ustack_phys) {
    pmm_free_pages(old_ustack_phys, USER_STACK_PAGES);
  }
  if (old_tls_phys) {
    pmm_free_page(old_tls_phys);
  }

  /* Free per-segment regions from the previous exec image (if any). */
  for (int i = 0; i < old_image.region_count; i++) {
    if (old_image.regions[i].phys && old_image.regions[i].pages) {
      pmm_free_pages(old_image.regions[i].phys, old_image.regions[i].pages);
    }
  }
  if (old_ttbr0) {
    /* Old user mappings were tagged with the old ASID. Invalidate them
     * before freeing the L1/L2/L3 pages so a future ASID rollover that
     * recycles the old value cannot resurrect entries pointing at pages
     * already returned to the PMM. */
    uint16_t old_asid = ttbr_asid(old_ttbr0);
    if (old_asid) {
      uint64_t arg = (uint64_t)old_asid << TTBR_ASID_SHIFT;
      __asm__ __volatile__("tlbi aside1, %0\n\t"
                           "dsb ish\n\t"
                           "isb"
                           :: "r"(arg));
    }
    mmu_free_user_tables((uint64_t *)ttbr_baddr(old_ttbr0));
  }

/* (diagnostic moved above the swap — `path` and old user state are gone
   here.) */
  return 0;
}


/*
 * mmap helpers
 *
 * The address-space policy lives here rather than in vma.c because it's a
 * policy question (where may user memory live?) and the VMA layer is a generic
 * bookkeeping structure with no opinions about it.
 */

/* Highest user VA this kernel will ever map. Anything above is rejected
 * before it can reach the page-table walk. */
#define MM_MAX_USER_VA USER_VA_TOP

/* Reject ranges that would escape the user window or wrap the address space.
 * Returns 1 if [start, end) is acceptable. */
static int mmap_range_ok(uint64_t start, uint64_t end) {
  if (start >= end) {
    return 0; /* zero-length or inverted */
  }
  if (end > MM_MAX_USER_VA) {
    return 0;
  }
  return 1;
}

/* Pick the lowest free aligned range of `len` bytes, scanning the VMA list.
 *
 * Bottom-up allocation keeps low addresses (where the kernel maps user text
 * and the stack) untouched and makes the layout predictable, which matters
 * when debugging. Returns 0 if no gap large enough exists below
 * MM_MAX_USER_VA. */
static uint64_t mmap_find_free(task_t *t, uint64_t len) {
  uint64_t candidate = USER_MMAP_BASE;

  while (candidate + len <= MM_MAX_USER_VA) {
    vma_t *hit = vma_find_overlap(&t->vmas, candidate, candidate + len);
    if (!hit) {
      return candidate;
    }
    /* Jump past the blocking region and re-align (start is already
     * page-aligned, so the +0 is redundant but keeps the invariant explicit
     * if USER_MMAP_BASE ever changes). */
    candidate = PAGE_ALIGN_UP(hit->end);
  }
  return 0;
}

/* SYS_MMAP — create a region. Anonymous only in this first implementation.
 *
 * Linux arg order: addr, length, prot, flags, fd, offset.
 *
 * Deliberate restrictions, each returning MAP_FAILED rather than pretending:
 *   - MAP_ANONYMOUS required; file-backed mapping needs a page cache that
 *     doesn't exist yet.
 *   - MAP_FIXED honored (with full range + overlap validation), since a libc
 *     allocator that wants a fixed arena should not be blocked.
 *   - MAP_SHARED REFUSED, not downgraded to private. With no page cache a
 *     shared anon region can't be made observable, and silently upgrading it
 *     would produce a program that appears to work and quietly loses data.
 *     EOPNOTSUPP is the honest answer: this kernel can't do it.
 */
static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
  task_t *t = sched_current();
  if (!t || !t->ttbr0) {
    return fail(EINVAL); /* MAP_FAILED */
  }

  /* Only prot bits we understand; anything else means the caller is using a
   * flag from a newer kernel and we should not guess. */
  if (prot & ~(uint64_t)(VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC)) {
    return fail(EINVAL);
  }
  if (flags & MAP_SHARED) {
    return fail(EOPNOTSUPP);
  }
  if (!(flags & MAP_ANONYMOUS)) {
    return fail(EOPNOTSUPP);
  }
  if (offset != 0 || fd != (uint64_t)-1) {
    /* Anonymous mappings ignore fd and must have offset 0. */
    return fail(EINVAL);
  }
  if (length == 0) {
    return fail(EINVAL);
  }

  /* Page-align the range. Linux rounds the length up; the start is truncated
   * down unless MAP_FIXED, in which case an unaligned addr is EINVAL. */
  uint64_t start;
  uint64_t end;

  if (flags & MAP_FIXED) {
    if (addr & (PAGE_SIZE - 1)) {
      return fail(EINVAL);
    }
    start = addr;
    end   = addr + length;
    if (!mmap_range_ok(start, end)) {
      return fail(EINVAL);
    }
    /* MAP_FIXED replaces whatever was there. For now only an untouched
     * region is acceptable — unmapping arbitrary overlapping regions means
     * splitting VMAs, which this implementation doesn't do. */
    if (vma_find_overlap(&t->vmas, start, end)) {
      return fail(EINVAL);
    }
  } else {
    uint64_t want = PAGE_ALIGN_UP(length);
    if (want < length) {
      return fail(ENOMEM); /* overflow */
    }
    start = mmap_find_free(t, want);
    if (!start) {
      return fail(ENOMEM); /* no space below USER_VA_TOP */
    }
    end = start + want;
    if (!mmap_range_ok(start, end)) {
      return fail(ENOMEM);
    }
  }

  vma_t *vma = vma_add(&t->vmas, start, end, (uint32_t)prot,
                       VMA_ANONYMOUS | VMA_SHARED);
  if (!vma) {
    /* VMA table full, or kmalloc failed. Either way the constraint is
     * address space rather than the arguments — ENOMEM, not EINVAL. */
    return fail(ENOMEM);
  }

  /* No page-table work happens here: the region is empty and pages are
   * faulted in on first touch by sched_try_handle_vma_fault(). For a large
   * mapping this means mmap() is O(1) in physical memory — pages only exist
   * once touched. */

  uart_printf("[MM] task %u '%s' mmap %x-%x prot=%u flags=%u -> VMA #%u\n",
              t->pid, t->name, start, end, prot, flags, vma_count(&t->vmas));
  return (int64_t)start;
}

/* SYS_MUNMAP — release a region, splitting VMAs as needed.
 *
 * Order matters: the VMA records and their physical pages are released BEFORE
 * the page-table entries are cleared. If a fault landed between the two we
 * could find a VMA for an address whose page we were midway through freeing.
 * The VMA list is authoritative for "may this task touch this address", so
 * it must be the first thing to change.
 *
 * Clearing the L3 descriptors is a pure address-range operation and doesn't
 * care how VMAs are shaped, which is why splitting can't desynchronize it. */
static int64_t sys_munmap(uint64_t addr, uint64_t length) {
  task_t *t = sched_current();
  if (!t || !t->ttbr0) {
    return fail(EINVAL);
  }
  if (addr & (PAGE_SIZE - 1) || length == 0) {
    return fail(EINVAL);
  }
  if (addr == 0) {
    return 0; /* POSIX: munmap(0, n) is a no-op */
  }

  /* Overflow guard before computing the end: a huge length would wrap the
   * sum into a small value that passes every range check below. */
  if (length > MM_MAX_USER_VA || addr > MM_MAX_USER_VA - length) {
    return fail(EINVAL);
  }

  /* Round to whole pages the Linux way: munmap(p, 1) unmaps the single page
   * containing p, so the end is derived from the last byte, not the length. */
  uint64_t end = PAGE_ALIGN_DOWN(addr + length - 1) + PAGE_SIZE;
  if (end > MM_MAX_USER_VA) {
    return fail(EINVAL);
  }

  int64_t released = vma_unmap_range(&t->vmas, addr, end);
  if (released < 0) {
    return fail(ENOMEM); /* only failure is running out of VMA slots */
  }

  uint64_t *l0 = (uint64_t *)ttbr_baddr(t->ttbr0);
  mmu_unmap_user_range(l0, addr, (end - addr) / PAGE_SIZE);

  uart_printf("[MM] task %u '%s' munmap %x-%x (%u pages, %u resident freed)\n",
              t->pid, t->name, addr, end, (end - addr) / PAGE_SIZE,
              released);
  return 0;
}

/* SYS_MPROTECT — change protection, splitting VMAs as needed. */
static int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
  task_t *t = sched_current();
  if (!t || !t->ttbr0) {
    return fail(EINVAL);
  }
  if (addr & (PAGE_SIZE - 1) || length == 0) {
    return fail(EINVAL);
  }
  if (prot & ~(uint64_t)(VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC)) {
    return fail(EINVAL);
  }
  if (addr == 0) {
    return 0;
  }

  /* Overflow guard, same reasoning as munmap. */
  if (length > MM_MAX_USER_VA || addr > MM_MAX_USER_VA - length) {
    return fail(EINVAL);
  }

  uint64_t end = PAGE_ALIGN_DOWN(addr + length - 1) + PAGE_SIZE;
  if (end > MM_MAX_USER_VA) {
    return fail(EINVAL);
  }

  /* vma_protect_range splits first so that every VMA intersecting [addr, end)
   * ends up ENTIRELY inside it. That postcondition is what makes the
   * address-range PTE rewrite below correct: mmu_protect_user_range can't
   * know which VMA a given page belonged to, so it can only be trusted once
   * the range aligns with VMA boundaries.
   *
   * Record first, then tables: if the table update is interrupted, pages
   * keep the OLD (possibly weaker) protection rather than being left mapped
   * with stronger protection than the VMA records claim. */
  int touched = vma_protect_range(&t->vmas, addr, end, (uint32_t)prot);
  if (touched < 0) {
    return fail(ENOMEM); /* out of VMA slots to split into */
  }

  /* Re-protect the already-faulted pages now, and record the new protection
   * so pages faulted in LATER get it too. Both halves matter: without the
   * table update a live page keeps its old permissions, and without the VMA
   * update a later fault reinstates the old permissions. */
  uint64_t *l0 = (uint64_t *)ttbr_baddr(t->ttbr0);
  mmu_protect_user_range(l0, addr, (end - addr) / PAGE_SIZE,
                         mmu_prot_to_flags((uint32_t)prot));

  uart_printf("[MM] task %u '%s' mprotect %x-%x -> prot=%u (%u VMAs)\n",
              t->pid, t->name, addr, end, prot, touched);
  return 0;
}

/* SYS_BRK — resize the program break, a contiguous heap.
 *
 * brk is a legacy interface: it hands out a single contiguous region whose
 * end the program moves with brk(addr). It predates mmap and glibc's default
 * allocator uses it via sbrk, so a libc still needs it even when mmap works.
 *
 * Implemented as a lazily-extended anonymous VMA: brk(0) reports the current
 * break, and brk(addr) grows the region up to addr (or shrinks it, which is
 * allowed but discouraged — freeing below the break is what free() is for).
 * Growth allocates NO physical memory; pages appear on first touch, same as
 * any other anonymous mapping. That's what makes a large brk affordable.
 *
 * The arena has a fixed reservation so growth is always contiguous and needs
 * no address search. Ceiling is USER_BRK_RESERVE_PAGES (see mmu.h).
 *
 * The break is BYTE-granular, matching Linux and POSIX, even though the
 * backing mapping is page-granular. That distinction is load-bearing: a libc
 * allocator asks for brk(break + 96) and then checks the returned break equals
 * what it requested. Returning a page-rounded value instead makes every such
 * call look like a failure and the allocator never gets off the ground. So the
 * VMA end is rounded up to a page while brk_cur stays exact.
 */
static int64_t sys_brk(uint64_t addr) {
  task_t *t = sched_current();
  if (!t || !t->ttbr0) {
    return (int64_t)t->brk_cur;
  }

  uint64_t lo = t->brk_base;
  uint64_t hi = t->brk_base + USER_BRK_RESERVE_PAGES * PAGE_SIZE;

  /* Linux returns the *current* break on failure rather than an errno, so a
   * caller can detect failure by comparing. Match that. */
  if (addr == 0) {
    return (int64_t)t->brk_cur;
  }
  if (addr < lo || addr > hi) {
    uart_printf("[BRK] task %u brk %x out of range [%x,%x]\n", t->pid, addr,
                lo, hi);
    return (int64_t)t->brk_cur;
  }

  /* Shrinking below the current break is legal but pointless; the pages stay
   * mapped so a stale pointer that later reads back doesn't fault. Keep the
   * break where it is in that case rather than unmapping. */
  if (addr < t->brk_cur) {
    return (int64_t)t->brk_cur;
  }

  /* Mapping extent is page-rounded; the break itself stays byte-exact. */
  uint64_t new_end = PAGE_ALIGN_UP(addr);
  uint64_t new_cur = addr;

  /* Grow the VMA to cover the new break. vma_add refuses overlap, so grow
   * from the current break only when the VMA doesn't already span it. */
  vma_t *vma = vma_find(&t->vmas, t->brk_cur);
  if (!vma) {
    /* No VMA yet — create the initial one. */
    if (new_end > lo) {
      vma_add(&t->vmas, lo, new_end, VMA_PROT_READ | VMA_PROT_WRITE,
              VMA_ANONYMOUS);
    }
  } else if (vma->end < new_end) {
    /* Extend the existing VMA. Rewriting [vma->start, new_end) in place is
     * simpler than unmap+add and keeps already-faulted pages mapped. */
    uint64_t grow = new_end - vma->end;
    uint64_t *bigger =
        (uintptr_t *)kmalloc(((vma->end - vma->start) / PAGE_SIZE + grow / PAGE_SIZE) *
                             sizeof(uintptr_t));
    if (!bigger) {
      return (int64_t)t->brk_cur; /* growth refused: report current break */
    }
    uint64_t old_n = (vma->end - vma->start) / PAGE_SIZE;
    memcpy(bigger, vma->pages, old_n * sizeof(uintptr_t));
    memset(bigger + old_n, 0, (grow / PAGE_SIZE) * sizeof(uintptr_t));
    kfree(vma->pages);
    vma->pages = bigger;
    vma->end  = new_end;
  }

  t->brk_cur = new_cur;
  return (int64_t)t->brk_cur;
}


/* Fill a Linux struct stat for `v`.
 *
 * Two things are deliberately absent rather than faked. Timestamps are all
 * zero because nothing records them: a plausible-looking mtime would be a
 * lie a program could act on (incremental builds, cache validation). And
 * st_blocks reports actual bytes, since that is what is on the medium, even
 * though a real filesystem reports allocated blocks. */
static void fill_linux_stat(vnode_t *v, struct linux_stat *st) {
memset(st, 0, sizeof(*st));

uint32_t type;
switch (v->type) {
  case VNODE_DIR: type = LNX_S_IFDIR; break;
  case VNODE_CHR: type = LNX_S_IFCHR; break;
  case VNODE_BLK: type = LNX_S_IFBLK; break;
  default:        type = LNX_S_IFREG; break;
}

/* Directories need the execute bit to be traversable, which every real
 * filesystem grants; without it a guest cannot descend into one at all. */
uint32_t perm = LNX_S_IRUSR | LNX_S_IWUSR | LNX_S_IRGRP | LNX_S_IROTH |
                LNX_S_IWGRP | LNX_S_IWOTH;
if (v->type == VNODE_DIR) {
  perm |= LNX_S_IXUSR | LNX_S_IXGRP | LNX_S_IXOTH;
}

st->st_mode    = type | perm;
/* One name reaches every vnode: this kernel has no hard links. Claiming more
 * would let a program believe a file survives an unlink it cannot perform. */
st->st_nlink   = 1;
st->st_ino     = (uint64_t)(uintptr_t)v;
st->st_dev     = 0; /* single filesystem, no device numbering */
st->st_uid     = 0;
st->st_gid     = 0;
st->st_size    = (int64_t)v->size;
st->st_blksize = PAGE_SIZE;
/* st_blocks counts 512-byte units on Linux, NOT bytes. */
st->st_blocks  = ((int64_t)v->size + 511) / 512;
}

/* Translate a Linux aarch64 syscall number onto the Fermi number the switch
 * below dispatches on.
 *
 * Two numbering schemes coexist on purpose. Our libc uses small, readable
 * numbers assigned as syscalls were added; a Linux binary uses the asm-generic
 * table and can never be made to speak ours. Aliasing keeps one table as the
 * single place that has to be correct for a foreign binary, which is far easier
 * to audit than a renumbering spread across every stub and test.
 *
 * Only numbers whose argument layout is IDENTICAL are aliased here. Anything
 * that shifts or reshapes arguments (openat, nanosleep, stat family) gets its
 * own case below instead, because an alias would silently read the wrong
 * argument.
 *
 * Numbers already equal (brk 214, munmap 215, mmap 222, mprotect 226,
 * wait4/waitpid 260) need no entry — they pass through unchanged.
 *
 * Unlisted Linux numbers fall through untouched and hit the ENOSYS default,
 * so an unimplemented call reports "not implemented" rather than aliasing onto
 * an unrelated Fermi syscall that happens to share the number. */
static uint64_t translate_linux_syscall(uint64_t num) {
  switch (num) {
  case LNX_READ:        return SYS_READ;
  case LNX_WRITE:       return SYS_WRITE;
  case LNX_CLOSE:       return SYS_CLOSE;
  case LNX_LSEEK:       return SYS_LSEEK;
  case LNX_EXIT:        return SYS_EXIT;
  case LNX_EXIT_GROUP:  return SYS_EXIT;
  case LNX_GETPID:      return SYS_GETPID;
  default:              return num;
  }
}

void syscall_dispatch(trap_frame_t *frame) {
  /* Allow preemption during the syscall. EL0 had IRQs unmasked, but the
   * synchronous-from-lower-EL vector entry implicitly masks them. Without
   * this, a long-blocking syscall (e.g. sys_read polling the UART RX
   * register inside the shell) would prevent the timer IRQ from firing
   * and starve every other task. */
  __asm__ __volatile__("msr daifclr, #2" ::: "memory");


  uint64_t num = frame->regs[8];

  /* A Linux binary's numbers are translated here; our libc's pass through
   * unchanged because translate_linux_syscall() returns anything it doesn't
   * know by identity. */
  num = translate_linux_syscall(num);
  uint64_t arg0 = frame->regs[0];
  uint64_t arg1 = frame->regs[1];
  uint64_t arg2 = frame->regs[2];
  uint64_t arg3 = frame->regs[3];
  uint64_t arg4 = frame->regs[4];
  uint64_t arg5 = frame->regs[5];

  int64_t ret = -1;
  task_t     *cur = sched_current();
  fd_table_t *fds = cur->fds;

  switch (num) {
  case SYS_READ:
    if (fds && user_buf_ok(arg1, (size_t)arg2)) {
      ret = fd_read(fds, (int)arg0, (void *)arg1, (size_t)arg2);
    } else {
      uart_errorln("[SYSCALL] SYS_READ rejected: bad user buffer");
    }
    break;

  case SYS_WRITE:
    if (fds && user_buf_ok(arg1, (size_t)arg2)) {
      ret = fd_write(fds, (int)arg0, (const void *)arg1, (size_t)arg2);
    } else {
      uart_errorln("[SYSCALL] SYS_WRITE rejected: bad user buffer");
    }
    break;

  case SYS_OPEN:
    /* Linux arg order: path, flags, mode. mode is only meaningful with
     * O_CREAT/O_TMPFILE and is accepted but not enforced (no ownership model
     * yet) — same as the VFS layer it calls into. */
    if (!fds || user_str_ok(arg0) < 0) {
      ret = fail(EFAULT);
      break;
    }
    ret = fd_open_ex(fds, (const char *)arg0, (int)arg1, (int)arg2);
    if (ret < 0) {
      /* Distinguish "no such file" from "exists but not permitted": stdio and
       * most libc wrappers branch on exactly this distinction when deciding
       * whether to fall back to creating a file. */
      vnode_t *probe = vfs_resolve((const char *)arg0);
      ret = fail(probe ? EACCES : (ENOENT));
    }
    break;

  case SYS_CLOSE:
    if (fds) {
      ret = fd_close(fds, (int)arg0);
    }
    break;

  case SYS_EXIT:
    /* arg0 is the exit status, handed to the parent via
     * waitpid(). A long's worth of bits keeps the ABI 64-bit clean. */
    task_exit((int)(long)arg0);
    break;

  case SYS_YIELD:
    schedule();
    ret = 0;
    break;

  case SYS_SLEEP:
    sleep_ms(arg0);
    ret = 0;
    break;

  case SYS_GETPID:
    /* Returns the calling task's pid. No arguments, no failure mode. */
    ret = (int64_t)sched_current()->pid;
    break;

  case SYS_LSEEK:
    /* arg0 = fd, arg1 = signed offset, arg2 = whence (SEEK_SET/CUR/END) */
    if (fds) {
      ret = fd_seek(fds, (int)arg0, (int64_t)arg1, (int)arg2);
    }
    break;

  case SYS_UPTIME:
    /* Milliseconds since boot. Uses the kernel's tick counter; resolution
     * is therefore TIMER_INTERVAL_MS (10 ms by default). */
    ret = (int64_t)timer_uptime_ms();
    break;


  case SYS_NET_PING: {
    /* arg0 = seq number. Sends one ICMP echo request to the slirp gateway
     * and busy-polls the RX queue for the matching reply. Returns the
     * reply's IP TTL on success, or -1 if no reply arrived in time.
     *
     * Race: shares net_rx_poll with kernel-mode netd. With single-CPU
     * scheduling and IRQ-driven preemption, both pollers may consume
     * frames the other expected. Both sides handle 'no reply' gracefully
     * so the worst outcome is an occasional spurious failure. */
    uint16_t seq = (uint16_t)arg0;
    if (net_send_ping(seq) <= 0) {
      ret = -1;
      break;
    }
    uint8_t buf[256];
    int got_ttl = -1;
    for (uint32_t spins = 0; spins < 2000000u && got_ttl < 0; spins++) {
      int n = net_rx_poll(buf, sizeof(buf));
      if (n < 14 + 20 + 8)                 continue;
      if (buf[12] != 0x08 || buf[13] != 0x00) continue; /* not IPv4 */
      const uint8_t *ip   = &buf[14];
      const uint8_t *icmp = &buf[14 + 20];
      if (ip[9] != 1 || icmp[0] != 0)      continue;     /* not ICMP echo reply */
      uint16_t reply_seq = ((uint16_t)icmp[6] << 8) | icmp[7];
      if (reply_seq != seq)                 continue;     /* not our reply */
      got_ttl = ip[8];
    }
    ret = (int64_t)got_ttl;
    break;
  }


  case SYS_KILL:
    /* arg0 = pid. Returns 0 on success or -1. */
    ret = (int64_t)sched_kill_task(arg0);
    break;

  case SYS_FORK:
    /* No arguments. Returns child pid to the caller; the child task,
     * when first scheduled, returns 0 from this same SVC via fork_return. */
    ret = (int64_t)sched_fork(sched_current(), frame);
    break;

  case SYS_WAITPID: {
    /* arg1 points at a user-space int for the status word. Validate it as a
     * user pointer before writing through it — an unchecked store to a
     * user-supplied address is an arbitrary-kernel-write primitive. */
    int   status    = 0;
    int  *statusptr = 0;
    if (arg1 != 0) {
      if (!user_buf_ok(arg1, sizeof(int))) {
        ret = fail(EFAULT);
        break;
      }
      statusptr = (int *)arg1;
    }
    long pid = sched_waitpid((int64_t)(long)arg0, &status, (int)arg2);
    if (pid < 0) {
      ret = fail(ECHILD);
    } else {
      if (statusptr) {
        *statusptr = status;
      }
      ret = pid;
    }
    break;
  }

  /* ---- Linux-only entry points ---------------------------------------
   *
   * These are reached before translation (their numbers mean something else to
   * our own libc, so they must not be aliased) and exist because every Linux
   * runtime calls them during startup. A binary that got these wrong would
   * fail before reaching its first line of real code.
   */

  case LNX_ARCH_PRCTL: {
    /* arch_prctl(ARCH_SET_FS, addr) is how a Linux aarch64 process installs
     * its thread pointer. We already give a task a TLS page at exec, but a
     * foreign binary expects to set FS itself and would otherwise run with a
     * stale pointer. ARCH_GET_FS reports it back.
     *
     * SET_GS is refused rather than ignored: silently accepting a register we
     * don't implement would let a guest believe it had shadow state. */
    if (arg0 == ARCH_SET_FS || arg0 == ARCH_GET_FS) {
      uint64_t *tpidr_el0;
      __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tpidr_el0));
      if (arg0 == ARCH_SET_FS) {
        if (arg1 == 0 || (arg1 & 7) != 0) {
          ret = fail(EINVAL);
          break;
        }
        __asm__ __volatile__("msr tpidr_el0, %0" ::"r"(arg1) : "memory");
        /* SET_FS returns 0 on success. Returning the register's PREVIOUS value
         * here looked harmless but every libc startup treats a non-zero return
         * from arch_prctl as fatal and aborts, so this must be exactly zero.
         * Only GET_FS reports a value. */
        ret = 0;
      } else {
        ret = (int64_t)(uintptr_t)tpidr_el0;
      }
    } else {
      ret = fail(EINVAL);
    }
    break;
  }

  case LNX_SET_TID_ADDRESS: {
    /* Every glibc/musl startup calls this to register where a clear_tid may be
     * written when a thread exits. There are no threads yet, so the address is
     * remembered as bookkeeping and nothing ever writes through it — but the
     * call must SUCCEED and return the caller's tid, because startup aborts on
     * a negative return. */
    cur->clear_tid = arg1;
    ret = (int64_t)cur->pid;
    break;
  }

  case LNX_OPENAT: {
    /* openat(dirfd, path, flags, mode) — the path is arg1, not arg0, which is
     * exactly why this isn't aliased onto SYS_OPEN.
     *
     * Only AT_FDCWD is accepted. Resolving a relative path against some other
     * fd would need per-descriptor working directories, which this VFS does
     * not track; accepting it and resolving against the wrong directory would
     * be worse than refusing. */
    /* Compare the low 32 bits: AT_FDCWD is a negative int, and a guest may
     * pass it zero-extended. Truncating matches how the kernel actually
     * interprets the argument. */
    if ((int32_t)arg0 != (int32_t)AT_FDCWD) {
      ret = fail(ENOSYS);
      break;
    }
    if (!fds || user_str_ok(arg1) < 0) {
      ret = fail(EFAULT);
      break;
    }
    ret = fd_open_ex(fds, (const char *)arg1, (int)arg2, (int)arg3);
    if (ret < 0) {
      vnode_t *probe = vfs_resolve((const char *)arg1);
      ret = fail(probe ? EACCES : ENOENT);
    }
    break;
  }

  case LNX_CLOCK_GETTIME: {
    /* clock_gettime(clockid, timespec). */
    if (arg0 != LNX_CLOCK_REALTIME && arg0 != LNX_CLOCK_MONOTONIC) {
      ret = fail(EINVAL);
      break;
    }
    if (!user_buf_ok(arg1, sizeof(struct linux_timespec))) {
      ret = fail(EFAULT);
      break;
    }
    struct linux_timespec ts;
    /* No wall clock exists yet -- there is no RTC and nothing reads one -- so
     * both clocks report time since boot. CLOCK_REALTIME naming boot as the
     * epoch is a limitation the caller cannot detect, so it is documented
     * rather than dressed up; CLOCK_MONOTONIC is exactly right. */
    ts.tv_sec  = (int64_t)timer_uptime_seconds();
    ts.tv_nsec = (int64_t)((timer_uptime_ms() % 1000) * 1000000ull);
    memcpy((void *)arg1, &ts, sizeof(ts));
    ret = 0;
    break;
  }

  case LNX_NANOSLEEP: {
    /* nanosleep(request, remaining). */
    if (!user_buf_ok(arg0, sizeof(struct linux_timespec))) {
      ret = fail(EFAULT);
      break;
    }
    struct linux_timespec req;
    memcpy(&req, (void *)arg0, sizeof(req));
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000ll) {
      ret = fail(EINVAL);
      break;
    }
    /* The timer ticks at 10 ms, so a sub-tick request is rounded UP. Rounding
     * down would return early and break callers that rely on sleeping at least
     * the interval asked for (retry backoff, pacing loops). */
    uint64_t ms = (uint64_t)req.tv_sec * 1000ull +
                  ((uint64_t)req.tv_nsec + 999999ull) / 1000000ull;
    if (ms) {
      sleep_ms(ms);
    }
    /* Nothing can interrupt a sleep -- there are no signals -- so the remainder
     * is always zero. Writing it keeps the contract; a real signal path will
     * have to fill it in properly. */
    if (arg1) {
      if (!user_buf_ok(arg1, sizeof(struct linux_timespec))) {
        ret = fail(EFAULT);
        break;
      }
      struct linux_timespec zero = {0, 0};
      memcpy((void *)arg1, &zero, sizeof(zero));
    }
    ret = 0;
    break;
  }

  case LNX_GETRANDOM: {
    /* getrandom(buf, buflen, flags). */
    if (arg1 == 0) {
      ret = 0;
      break;
    }
    if (!user_buf_ok(arg0, (size_t)arg1)) {
      ret = fail(EFAULT);
      break;
    }
    /* GRND_NONBLOCK and GRND_RANDOM are accepted and ignored: both only request
     * behaviour this implementation already has (blocking, any source), so
     * honouring them by doing nothing is accurate rather than a pretence. */
    int64_t got = rng_read((void *)arg0, (uint32_t)arg1);
    if (got < 0) {
      /* No entropy device. Reporting failure is the right call here: a caller
       * seeding a key from a silently-zeroed buffer would be far worse off
       * than one that knows randomness was unavailable. */
      ret = fail(EIO);
      break;
    }
    ret = got;
    break;
  }

  case LNX_FSTAT: {
    /* fstat(fd, statbuf). */
    vnode_t *v = fds ? fd_vnode(fds, (int)arg0) : NULL;
    if (!v) {
      ret = fail(EBADF);
      break;
    }
    if (!user_buf_ok(arg1, sizeof(struct linux_stat))) {
      ret = fail(EFAULT);
      break;
    }
    struct linux_stat st;
    fill_linux_stat(v, &st);
    memcpy((void *)arg1, &st, sizeof(st));
    ret = 0;
    break;
  }

  case LNX_NEWFSTATAT: {
    /* newfstatat(dirfd, path, statbuf, flags). */
    if ((int32_t)arg0 != (int32_t)AT_FDCWD) {
      ret = fail(ENOSYS);
      break;
    }
    /* AT_SYMLINK_NOFOLLOW (0x100) is accepted and has no effect: this VFS has
     * no symlinks, so there is nothing to not-follow. Any other flag bit is
     * rejected instead of ignored, because ignoring one silently could return
     * the wrong file's metadata. */
    if (!user_str_ok(arg1) || !user_buf_ok(arg2, sizeof(struct linux_stat))) {
      ret = fail(EFAULT);
      break;
    }
    if (arg3 & ~(uint64_t)0x100) {
      ret = fail(EINVAL);
      break;
    }
    vnode_t *v = vfs_resolve((const char *)arg1);
    if (!v) {
      ret = fail(ENOENT);
      break;
    }
    struct linux_stat st;
    fill_linux_stat(v, &st);
    memcpy((void *)arg2, &st, sizeof(st));
    ret = 0;
    break;
  }

  case SYS_MMAP:
    /* Linux arg order: addr, length, prot, flags, fd, offset. */
    ret = sys_mmap(arg0, arg1, arg2, arg3, arg4, arg5);
    break;

  case SYS_BRK:
    ret = sys_brk(arg0);
    break;

  case SYS_MUNMAP:
    ret = sys_munmap(arg0, arg1);
    break;

  case SYS_MPROTECT:
    ret = sys_mprotect(arg0, arg1, arg2);
    break;

  case SYS_BALLOON: {
    /* arg0 = op (BALLOON_OP_*), arg1 = page count. Status ops ignore arg1
     * and return the relevant counter. Inflate/deflate return how many
     * pages actually moved. */
    uint32_t actual = 0, target = 0;
    switch (arg0) {
    case BALLOON_OP_INFLATE:
      ret = (int64_t)balloon_inflate((uint32_t)arg1);
      break;
    case BALLOON_OP_DEFLATE:
      ret = (int64_t)balloon_deflate((uint32_t)arg1);
      break;
    case BALLOON_OP_ACTUAL:
      balloon_get_status(&actual, NULL);
      ret = (int64_t)actual;
      break;
    case BALLOON_OP_TARGET:
      balloon_get_status(NULL, &target);
      ret = (int64_t)target;
      break;
    default:
      ret = -1;
      break;
    }
    break;
  }


  case SYS_EXEC: {
    /* arg0 = path, arg1 = argv. On success the trap frame has been fully
     * rewritten so the eret lands in the new program with x0=argc, x1=argv.
     * We MUST NOT fall through to the dispatcher's frame->regs[0] writeback
     * — that would clobber argc. Return early so the eret epilogue uses
     * the frame as-is. On failure (-1), surface that to the caller via the
     * normal x0 return path. */
    int64_t r = sys_exec(arg0, arg1, frame);
    if (r >= 0) {
      return;
    }
    ret = -1;
    break;
  }



  default:
    /* ENOSYS is what lets a libc feature-probe: a wrapper that returns
     * ENOSYS knows to fall back, where -1 with no reason is unusable. */
    uart_printf("[SYSCALL] Unknown syscall %u\n", num);
    ret = fail(ENOSYS);
    break;
  }
  // write return value back into trap frame
  // x0 is restored with the result on eret
  frame->regs[0] = (uint64_t)ret;
}
