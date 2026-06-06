#include "sched.h"
#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "timer/timer.h"
#include "uart/uart.h"
#include "vfs/vfs.h"

// Marks the page-aligned upper bound of read-only kernel pages that are safe
// to expose to EL0 (.text + .rodata). Defined by the linker script.
extern uint8_t __text_start[];
extern uint8_t __user_text_end[];


// switch.S: unmasks IRQs, calls x19, then calls task_exit
extern void task_trampoline(void);
extern void kernel_task_trampoline(void);

// Idle task is statically allocated, always exists
static task_t idle_task;
static task_t *current = &idle_task;
static uint64_t next_pid = 0;

// Dead tasks scheduled for cleanup (singly-linked list)
static task_t *dead_list = (void *)0;

/* ASID allocator. ASID 0 is reserved (used by tasks with ttbr0 == 0, like
 * idle and EL1 kernel tasks). User tasks consume ASIDs 1..65535.
 *
 * On wraparound from 65535 to 0 we flush the entire TLB and restart at 1.
 * Reused ASIDs after the flush cannot alias stale TLB entries because the
 * flush evicted everything. The 65535-task budget is never exhausted in
 * practice — fermi-os runs a handful of tasks for the whole boot session. */
static uint16_t next_asid = 1;

/* Demand-paged stack growth. Called from the EL0 data-abort handler when a
 * user task faults on an address below its currently-mapped stack but inside
 * the maximum stack range. We allocate one fresh page, zero it, and map it
 * with EL0 RW + UXN | PXN. The faulting instruction will re-execute and
 * succeed because the new translation is now installed.
 *
 * TLB invalidation: ARMv8 architecturally permits caching of failed
 * translations, so even though the prior fault implies "no entry", we
 * issue tlbi vae1 for safety on the affected (VA, ASID) pair before the
 * user re-tries the access. The dsb ish sequences the page-table store
 * before the invalidate; isb sequences the invalidate before eret.
 *
 * Returns 1 on success, 0 if `far` is outside the growth zone, the cap
 * is exhausted, or the PMM is empty. */
int sched_try_grow_stack(task_t *t, uint64_t far) {
  if (!t || !t->ttbr0) {
    return 0;
  }

  /* Bounds: far must be in [stack_max_lo, stack_initial_lo). Below
   * stack_max_lo is a wild pointer; at-or-above stack_initial_lo means
   * the page is already (or should already be) mapped — a fault there
   * is a permission/alignment issue, not a growth case. */
  uint64_t stack_max_lo     = USER_STACK_TOP - USER_STACK_PAGES_MAX * PAGE_SIZE;
  uint64_t stack_initial_lo = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
  if (far < stack_max_lo || far >= stack_initial_lo) {
    return 0;
  }

  if (t->stack_grown_count >= USER_STACK_GROWN_MAX) {
    uart_errorln("[STACK] grow refused: USER_STACK_GROWN_MAX hit");
    return 0;
  }

  /* Allocate + zero. memset goes through TTBR1 (PHYS_TO_VIRT) so we don't
   * depend on TTBR0 happening to identity-map RAM. */
  uintptr_t pa = pmm_allocate_page();
  if (!pa) {
    uart_errorln("[STACK] grow refused: PMM exhausted");
    return 0;
  }
  memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);

  uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  uint64_t *l0 = (uint64_t *)ttbr_baddr(t->ttbr0);
  mmu_map_user_range(l0, va, pa, 1, flags);

  /* tlbi vae1, Xt: Xt[63:48] = ASID, Xt[43:0] = VA[55:12] (page-frame
   * number). Only the (VA, ASID) pair we just mapped — cheap. */
  uint16_t asid = ttbr_asid(t->ttbr0);
  uint64_t arg  = ((uint64_t)asid << TTBR_ASID_SHIFT) | (va >> 12);
  __asm__ __volatile__("dsb ish\n\t"
                       "tlbi vae1, %0\n\t"
                       "dsb ish\n\t"
                       "isb"
                       :: "r"(arg));

  t->stack_grown_phys[t->stack_grown_count++] = pa;

  uart_printf("[STACK] grew task %u '%s' by 1 page at %x (now %u dyn pages)\n",
              t->pid, t->name, va, (uint64_t)t->stack_grown_count);
  return 1;
}


uint16_t sched_asid_alloc(void) {
  uint16_t a = next_asid++;
  if (next_asid == 0) {
    /* Counter wrapped past 65535. Flush every TLB entry (global and
     * non-global) so the next batch of recycled ASIDs starts cold. */
    __asm__ __volatile__("tlbi vmalle1\n\t"
                         "dsb ish\n\t"
                         "isb");
    next_asid = 1;
    uart_println("[SCHED] ASID space wrapped \u2014 flushed all TLBs");
  }
  return a;
}

static void copy_name(char *dst, const char *src) {
  for (int i = 0; src[i] && i < 15; i++) {
    dst[i] = src[i];
  }
}

void sched_init(void) {
  uart_println("[SCHED] Initializing scheduler");

  memset(&idle_task, 0, sizeof(task_t));
  idle_task.pid = next_pid++;
  // TASK_READY (not TASK_RUNNING): schedule() needs idle to be a READY
  // candidate so it can be picked as a fallback when no other task is
  // runnable. The transition to TASK_RUNNING happens lazily inside
  // schedule() once it's actually selected. Without this, calling
  // sleep_ms before the first preemption returned immediately because
  // the scheduler couldn't pick idle as a fallback.
  idle_task.state = TASK_READY;
  idle_task.stack_phys = 0;    // kernel stack, not PMM-managed
  idle_task.next = &idle_task; // circular: points to itself
  copy_name(idle_task.name, "idle");

  current = &idle_task;

  uart_println("[SCHED] Initialized! Idle task registered");
}

/* Create an EL1 kernel-mode task. Mirrors sched_create_task minus the
 * user_l0 / user-stack / fd-table setup; trampoline is
 * kernel_task_trampoline (no eret to EL0). */
int sched_create_kernel_task(const char *name, task_entry_t entry) {
  task_t *t = (task_t *)kmalloc(sizeof(task_t));
  if (!t) {
    uart_errorln("[SCHED] Failed to allocate kernel task struct");
    return -1;
  }
  memset(t, 0, sizeof(task_t));

  uintptr_t kstack_phys = pmm_allocate_pages(TASK_STACK_PAGES);
  if (!kstack_phys) {
    uart_errorln("[SCHED] Failed to allocate kernel-task stack");
    kfree(t);
    return -1;
  }
  uintptr_t kstack_va = PHYS_TO_VIRT(kstack_phys);
  uint64_t kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
  uintptr_t kstack_top = kstack_va + kstack_size;
  memset((void *)kstack_va, 0, kstack_size);

  /* Same 160-byte frame layout as user tasks. d8–d15 stay zero. */
  uint64_t *frame = (uint64_t *)(kstack_top - 160);
  frame[0]  = (uint64_t)entry;                  /* x19 — entry function    */
  frame[1]  = 0;                                /* x20 — unused for kernel */
  frame[11] = (uint64_t)kernel_task_trampoline; /* x30 — trampoline        */

  t->sp = (uint64_t)frame;
  t->pid = next_pid++;
  t->state = TASK_READY;
  t->stack_phys = kstack_phys;
  /* No user mappings — context_switch skips TTBR0 swap when ttbr0 == 0. */
  t->ttbr0 = 0;
  t->user_sp = 0;
  t->kstack_top = kstack_top;
  t->ustack_phys = 0;
  copy_name(t->name, name);
  /* Kernel tasks talk to the VFS / drivers directly; no fd table. */
  t->fds = (struct fd_table *)0;

  /* Insert into the circular run queue */
  task_t *tail = current;
  while (tail->next != current) {
    tail = tail->next;
  }
  tail->next = t;
  t->next = current;

  uart_printf(
      "[SCHED] Created EL1 kernel task %d '%s' | kstack: %x | entry: %x\n",
      t->pid, name, kstack_top, (uint64_t)entry);
  return (int)t->pid;
}


int sched_create_task(const char *name, task_entry_t entry) {
  task_t *t = (task_t *)kmalloc(sizeof(task_t));
  if (!t) {
    uart_errorln("[SCHED] Failed to allocate task struct");
    return -1;
  }
  memset(t, 0, sizeof(task_t));

  // Kernel stack (used during exceptions and context_switch)
  uintptr_t kstack_phys = pmm_allocate_pages(TASK_STACK_PAGES);
  if (!kstack_phys) {
    uart_errorln("[SCHED] Failed to allocate kernel stack");
    kfree(t);
    return -1;
  }
  uintptr_t kstack_va = PHYS_TO_VIRT(kstack_phys);
  uint64_t kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
  uintptr_t kstack_top = kstack_va + kstack_size;
  memset((void *)kstack_va, 0, kstack_size);

  // User page tables (TTBR0)
  uint64_t *user_l0 = mmu_create_user_tables();
  if (!user_l0) {
    uart_errorln("[SCHED] Failed to create user page tables");
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }

  // Map the entire .text + .rodata range [__text_start, __user_text_end)
  // as a single contiguous window starting at USER_TEXT_BASE. Every EL0
  // task gets the same mapping shape, regardless of where in .text its
  // entry function happens to land. This is required because GCC's
  // PC-relative ADRP+ADD can reach pages BEFORE the entry function (e.g.
  // helpers like memset that the linker placed earlier in .text); a
  // narrower window starting at the entry's page would fault on those.
  // .data and .bss remain kernel-private (linker-enforced).
  uint64_t entry_pa         = VIRT_TO_PHYS((uint64_t)entry);
  uint64_t text_start_pa    = VIRT_TO_PHYS((uint64_t)__text_start);
  uint64_t user_text_end_pa = VIRT_TO_PHYS((uint64_t)__user_text_end);

  if (entry_pa < text_start_pa || entry_pa >= user_text_end_pa) {
    uart_errorln("[SCHED] entry outside [__text_start, __user_text_end)");
    pmm_free_page((uintptr_t)user_l0);
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }
  uint64_t text_pages = (user_text_end_pa - text_start_pa) / PAGE_SIZE;
  uint64_t entry_offset_in_text = entry_pa - text_start_pa;

  // EL0 read+execute, kernel cannot execute (PXN).
  // AP=11 → EL0 RO, EL1 RO. UXN bit cleared → EL0 may execute.
  uint64_t text_flags = PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_PXN;
  mmu_map_user_range(user_l0, USER_TEXT_BASE, text_start_pa,
                     text_pages, text_flags);

  uint64_t user_entry = USER_TEXT_BASE + entry_offset_in_text;

  // User stack
  uintptr_t ustack_phys = pmm_allocate_pages(USER_STACK_PAGES);
  if (!ustack_phys) {
    uart_errorln("[SCHED] Failed to allocate user stack");
    // user_l0 has the text mapping populated; tear down its L1/L2/L3 tables
    // along with the L0 itself so we don't leak page-table pages.
    mmu_free_user_tables(user_l0);
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }
  uintptr_t ustack_va_kern = PHYS_TO_VIRT(ustack_phys);
  memset((void *)ustack_va_kern, 0, USER_STACK_PAGES * PAGE_SIZE);

  // Map user stack into user address space at USER_STACK_TOP - size
  uint64_t ustack_user_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  uint64_t stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  mmu_map_user_range(user_l0, ustack_user_base, ustack_phys,
                     USER_STACK_PAGES, stack_flags);

  // Set up initial kernel stack frame for context_switch.
  // Frame size matches the layout in switch.S: 12 GPRs (x19–x30) + 8 SIMD
  // (d8–d15) = 160 bytes. d8–d15 are left zero (a fresh task has no FP
  // state to preserve); the surrounding memset already zeroed the kstack.
  // task_trampoline will eret to EL0 using x19=user_entry, x20=user_sp.
  uint64_t *frame = (uint64_t *)(kstack_top - 160);
  frame[0] = user_entry;     // x19 — user entry point
  frame[1] = USER_STACK_TOP; // x20 — user SP
  frame[11] =
      (uint64_t)task_trampoline; // x30 (lr) — already TTBR1 VA with -fno-pic

  t->sp = (uint64_t)frame;
  t->pid = next_pid++;
  t->state = TASK_READY;
  t->stack_phys = kstack_phys;
  /* Pack ASID into TTBR0[63:48] so per-task TLB entries (mapped with
   * nG=1 by mmu_map_user_range) are isolated from every other task's. */
  t->ttbr0 = ttbr_pack((uint64_t)user_l0, sched_asid_alloc());
  t->user_sp = USER_STACK_TOP;
  t->kstack_top = kstack_top;
  t->ustack_phys = ustack_phys;
  copy_name(t->name, name);

  /* Per-task fd table: open /dev/console for fd 0, 1, 2 */
  t->fds = fd_table_create();
  if (t->fds) {
    fd_open(t->fds, "/dev/console"); /* stdin  = 0 */
    fd_open(t->fds, "/dev/console"); /* stdout = 1 */
    fd_open(t->fds, "/dev/console"); /* stderr = 2 */
  }

  // Insert into circular run queue
  task_t *tail = current;
  while (tail->next != current) {
    tail = tail->next;
  }
  tail->next = t;
  t->next = current;

  uart_printf(
      "[SCHED] Created EL0 task %d '%s' | kstack: %x | user_entry: %x\n",
      t->pid, name, kstack_top, user_entry);

  return (int)t->pid;
}

/* Deep-copy the calling task into a new child task. Mirrors the layout
 * sched_create_task uses, but populates the kstack with a snapshot of the
 * parent's trap frame so the child resumes from inside the same SVC.
 *
 * Returns the child's pid to the parent. The child sees x0 = 0 because
 * we explicitly clobber the saved x0 in its frame copy.
 *
 * Memory model:
 *   - .text / .rodata window is shared (read-only, EL0 RO + PXN). Same
 *     physical pages get re-mapped at the same VA in the child's L0.
 *   - User stack pages are *copied* — fresh PMM allocation, byte-for-byte
 *     memcpy from parent's stack region.
 *   - fd table is fresh (stdin/stdout/stderr to /dev/console). POSIX-true
 *     fd duplication is left as a TODO; current shell-fork-then-exit
 *     scenarios don't depend on inherited fds.
 */
int sched_fork(task_t *parent, struct trap_frame *frame) {
  if (!parent || !frame) {
    return -1;
  }

  task_t *t = (task_t *)kmalloc(sizeof(task_t));
  if (!t) {
    uart_errorln("[FORK] Failed to allocate child task struct");
    return -1;
  }
  memset(t, 0, sizeof(task_t));

  /* Child kernel stack. */
  uintptr_t kstack_phys = pmm_allocate_pages(TASK_STACK_PAGES);
  if (!kstack_phys) {
    uart_errorln("[FORK] Failed to allocate child kernel stack");
    kfree(t);
    return -1;
  }
  uintptr_t kstack_va  = PHYS_TO_VIRT(kstack_phys);
  uint64_t  kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
  uintptr_t kstack_top = kstack_va + kstack_size;
  memset((void *)kstack_va, 0, kstack_size);

  /* Child user-space page table. */
  uint64_t *user_l0 = mmu_create_user_tables();
  if (!user_l0) {
    uart_errorln("[FORK] Failed to allocate child user_l0");
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }

  /* Re-map the shared .text + .rodata window with the same physical pages.
   * Same flags / shape as sched_create_task; safe because the pages are
   * read-only at EL0 (PTE_AP_RO_EL0 + PTE_PXN). */
  uint64_t text_start_pa    = VIRT_TO_PHYS((uint64_t)__text_start);
  uint64_t user_text_end_pa = VIRT_TO_PHYS((uint64_t)__user_text_end);
  uint64_t text_pages       = (user_text_end_pa - text_start_pa) / PAGE_SIZE;
  uint64_t text_flags       = PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_PXN;
  mmu_map_user_range(user_l0, USER_TEXT_BASE, text_start_pa, text_pages,
                     text_flags);

  /* Fresh user stack — allocate physical pages and copy parent's contents. */
  uintptr_t ustack_phys = pmm_allocate_pages(USER_STACK_PAGES);
  if (!ustack_phys) {
    uart_errorln("[FORK] Failed to allocate child user stack");
    mmu_free_user_tables(user_l0);
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }
  memcpy((void *)PHYS_TO_VIRT(ustack_phys),
         (const void *)PHYS_TO_VIRT(parent->ustack_phys),
         USER_STACK_PAGES * PAGE_SIZE);

  uint64_t ustack_user_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  uint64_t stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  mmu_map_user_range(user_l0, ustack_user_base, ustack_phys, USER_STACK_PAGES,
                     stack_flags);

  /* ---- Lay out child's kstack ---------------------------------------- */
  /* Trap frame: 688 bytes (matches FRAME_SIZE in vector.S — GPRs + sysregs
   * + SP_EL0 + q0-q7 + q16-q31 + FPSR + FPCR). The whole frame is memcpy\x27d
   * verbatim so the child resumes inside the same SVC with parent\x27s full
   * register + FP state; we then clobber x0 = 0 to distinguish it from
   * the parent\x27s view of the syscall return. */
  uint8_t  *child_frame_bytes = (uint8_t *)(kstack_top - 688);
  memcpy(child_frame_bytes, (const uint8_t *)frame, 688);
  /* Child's saved x0 = 0  → distinguishes from parent (which sees pid). */
  ((uint64_t *)child_frame_bytes)[0] = 0;

  /* context_switch frame: 12 GPRs + 8 SIMD = 160 bytes, just below the
   * trap frame. x30 = fork_return so the first ret out of context_switch
   * lands there; x19/x20 unused (fork_return doesn't touch them). */
  uint64_t *switch_frame = (uint64_t *)((uintptr_t)child_frame_bytes - 160);
  switch_frame[0]  = 0;                       /* x19 */
  switch_frame[1]  = 0;                       /* x20 */
  switch_frame[11] = (uint64_t)fork_return;   /* x30 */
  /* d8–d15 (offsets 96–152) are already zero from the kstack memset. */

  /* ---- task_t fields -------------------------------------------------- */
  copy_name(t->name, parent->name);
  /* Append "+f" if there's room — cosmetic, helps `ps` distinguish them. */
  int nlen = 0;
  while (nlen < 16 && t->name[nlen] != '\0') nlen++;
  if (nlen + 2 < 16) {
    t->name[nlen]     = '+';
    t->name[nlen + 1] = 'f';
    t->name[nlen + 2] = '\0';
  }

  t->sp          = (uint64_t)switch_frame;
  t->pid         = next_pid++;
  t->state       = TASK_READY;
  t->stack_phys  = kstack_phys;
  /* Fresh ASID for the child — separate from parent's so each task has
   * its own TLB context. The parent's TLB entries stay valid under its
   * own ASID until it gets context-switched back in. */
  t->ttbr0       = ttbr_pack((uint64_t)user_l0, sched_asid_alloc());
  t->user_sp     = parent->user_sp;
  t->kstack_top  = kstack_top;
  t->ustack_phys = ustack_phys;

  /* Fresh fd table for the child. POSIX-correct fork would dup all fds;
   * for the MVP we give the child a clean stdin/stdout/stderr backed by
   * /dev/console (same as a brand-new task). */
  t->fds = fd_table_create();
  if (t->fds) {
    fd_open(t->fds, "/dev/console");
    fd_open(t->fds, "/dev/console");
    fd_open(t->fds, "/dev/console");
  }

  /* Insert into circular run queue (mask IRQs while we mutate it). */
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #2");

  task_t *tail = current;
  while (tail->next != current) {
    tail = tail->next;
  }
  tail->next = t;
  t->next    = current;

  __asm__ __volatile__("msr daif, %0" ::"r"(daif));

  uart_printf("[FORK] %d '%s' -> child %d '%s'\n",
              parent->pid, parent->name, t->pid, t->name);
  return (int)t->pid;
}


void schedule(void) {
  sched_reap();

  task_t *prev = current;
  task_t *next = prev->next;
  task_t *fallback = (void *)0; // idle fallback

  while (next != prev) {
    if (next->state == TASK_READY) {
      if (next == &idle_task) {
        if (!fallback) {
          fallback = next;
        }
        next = next->next;
        continue;
      }
      break;
    }
    next = next->next;
  }

  if (next == prev) {
    // prev is still runnable — no point switching to idle, let it keep its
    // timeslice
    if (!fallback || prev->state == TASK_RUNNING) {
      return;
    }
    // prev is dead/blocked — must switch to idle
    next = fallback;
  }

  if (prev->state == TASK_RUNNING) {
    prev->state = TASK_READY;
  }

  // If prev is dead, unlink from run queue and defer cleanup to sched_reap()
  if (prev->state == TASK_DEAD) {
    task_t *p = prev;
    while (p->next != prev) {
      p = p->next;
    }
    p->next = prev->next;

    prev->next = dead_list;
    dead_list = prev;
  }

  next->state = TASK_RUNNING;
  current = next;

  context_switch(prev, next);
}

void yield(void) {
  // voluntary preemption
  schedule();
}

void task_exit(void) {
  uart_printf("[SCHED] Task %d '%s' exiting\n", current->pid, current->name);

  // Mark dead — schedule() will unlink and push onto dead_list
  current->state = TASK_DEAD;

  // Switch away permanently, the current task is still on the dying task's
  // stack, so it cannot freed here. The idle loop calls sched_reap().
  schedule();
}

int sched_kill_task(uint64_t pid) {
  /* Refuse to kill the idle task — the scheduler relies on it as a
   * fallback when nothing else is runnable. */
  if (pid == 0) {
    uart_errorln("[SCHED] kill: refusing to kill idle (pid 0)");
    return -1;
  }

  task_t *head = sched_first_task();
  task_t *t    = head;
  do {
    if (t->pid != pid) {
      t = t->next;
      continue;
    }
    if (t->state == TASK_DEAD) {
      return -1; /* already gone */
    }
    if (t == current) {
      /* Killing self — use the existing self-exit path so the trap frame
       * on our own kstack is unwound naturally. */
      task_exit();
      return 0;
    }

    /* Mask IRQs while we mutate the run queue + dead_list to avoid
     * racing with timer-driven schedule(). */
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    __asm__ __volatile__("msr daifset, #2");

    uart_printf("[SCHED] Killing task %d '%s'\n", pid, t->name);

    /* Find predecessor in the circular list. caller != t guaranteed. */
    task_t *p = t;
    while (p->next != t) {
      p = p->next;
    }
    p->next = t->next;

    /* Push onto dead_list for sched_reap to clean up next idle pass. */
    t->next   = dead_list;
    dead_list = t;
    t->state  = TASK_DEAD;

    __asm__ __volatile__("msr daif, %0" ::"r"(daif));
    return 0;
  } while (t != head);

  return -1; /* pid not found */
}


void sleep_ms(uint64_t ms) {
  uint64_t interval_ms = TIMER_INTERVAL_MS;
  uint64_t ticks_needed = ms / interval_ms;
  if (ticks_needed == 0) {
    ticks_needed = 1;
  }

  current->sleep_until = timer_get_ticks() + ticks_needed;
  current->state = TASK_SLEEPING;

  uart_printf("[SCHED] Task %d '%s' sleeping for %u ms (%u ticks)\n",
              current->pid, current->name, ms, ticks_needed);

  schedule();
}

void sched_wake_sleepers(void) {
  uint64_t now = timer_get_ticks();
  task_t *t = idle_task.next;

  while (t != &idle_task) {
    if (t->state == TASK_SLEEPING && now >= t->sleep_until) {
      t->state = TASK_READY;
      t->sleep_until = 0;
    }
    t = t->next;
  }
}

void sched_reap(void) {
  while (dead_list) {
    task_t *dead = dead_list;
    dead_list = dead->next;

    uart_printf("[SCHED] Reaping task %d '%s'\n", dead->pid, dead->name);

    // Free kernel stack
    if (dead->stack_phys) {
      pmm_free_pages(dead->stack_phys, TASK_STACK_PAGES);
    }

    // Free user stack
    if (dead->ustack_phys) {
      pmm_free_pages(dead->ustack_phys, USER_STACK_PAGES);
    }

    /* Free per-exec PMM regions if this task was loaded by exec(). Tasks
     * running the kernel-shared image have region_count == 0. Each region
     * was a separate pmm_allocate_pages() call in elf_load(). */
    for (int i = 0; i < dead->exec_image.region_count; i++) {
      uintptr_t p  = dead->exec_image.regions[i].phys;
      uint64_t  pp = dead->exec_image.regions[i].pages;
      if (p && pp) {
        pmm_free_pages(p, pp);
      }
    }


    /* Free pages acquired through demand-paged stack growth.
     * mmu_free_user_tables walks intermediate L1/L2/L3 tables but not
     * leaf data pages — those are tracked separately so we can pmm_free
     * exactly what was pmm_allocate'd in sched_try_grow_stack. */
    for (int i = 0; i < dead->stack_grown_count; i++) {
      if (dead->stack_grown_phys[i]) {
        pmm_free_page(dead->stack_grown_phys[i]);
      }
    }


    /* Invalidate any TLB entries left behind by this task's ASID before we
     * free its page tables. Without this, a future ASID rollover that
     * recycles the same value could see lingering entries pointing at
     * physical pages that have been pmm_free'd and reallocated. */
    if (dead->ttbr0) {
      uint16_t dead_asid = ttbr_asid(dead->ttbr0);
      if (dead_asid) {
        /* tlbi aside1, Xt: ASID lives in Xt[63:48], same layout as TTBR0. */
        uint64_t arg = (uint64_t)dead_asid << TTBR_ASID_SHIFT;
        __asm__ __volatile__("tlbi aside1, %0\n\t"
                             "dsb ish\n\t"
                             "isb"
                             :: "r"(arg));
      }
      mmu_free_user_tables((uint64_t *)ttbr_baddr(dead->ttbr0));
    }

    // Free fd table
    if (dead->fds) {
      fd_table_destroy(dead->fds);
    }

    kfree(dead);
  }
}

task_t *sched_current(void) { return current; }

task_t *sched_first_task(void) { return &idle_task; }

const char *task_state_name(task_state_t s) {
  switch (s) {
  case TASK_READY:
    return "READY";
  case TASK_RUNNING:
    return "RUNNING";
  case TASK_SLEEPING:
    return "SLEEPING";
  case TASK_DEAD:
    return "DEAD";
  default:
    return "?";
  }
}
