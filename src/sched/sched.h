#ifndef SCHED_H
#define SCHED_H

#include "syscall/elf.h"  /* elf_image_t for per-task region tracking */
#include "mm/vma/vma.h"   /* VMA set — user address-space bookkeeping */
#include <stdint.h>

/* forward decl */
struct fd_table;

#define TASK_STACK_PAGES 4 // 16 KiB per task (kernel stack)
#define USER_STACK_PAGES 4 // 16 KiB user stack
/* Hard cap for demand-paged user stack growth. The initial USER_STACK_PAGES
 * are eagerly mapped at task creation; faults in [USER_STACK_TOP - MAX*PAGE,
 * USER_STACK_TOP - INITIAL*PAGE) trigger sched_try_grow_stack() which maps
 * a fresh page on demand up to this cap. 64 pages = 256 KiB total stack,
 * matching a typical Linux RLIMIT_STACK of ~8 MiB scaled to a hobby-kernel
 * memory budget. */
#define USER_STACK_PAGES_MAX 64
#define USER_STACK_GROWN_MAX (USER_STACK_PAGES_MAX - USER_STACK_PAGES)

typedef enum {
  TASK_READY,
  TASK_RUNNING,
  TASK_SLEEPING,
  TASK_DEAD
} task_state_t;

typedef void (*task_entry_t)(void);

typedef struct task {
  uint64_t sp; // offset 0: kernel SP (context_switch saves/restores here)
  uint64_t pid;
  task_state_t state;
  uint64_t sleep_until;
  uintptr_t stack_phys;
  // EL0 user-space fields
  uint64_t ttbr0;        // user page table base (physical address)
  uint64_t user_sp;      // SP_EL0 — user stack pointer
  uintptr_t kstack_top;  // top of per-task kernel stack (for exception entry)
  uintptr_t ustack_phys; // physical address of user stack (for freeing)
  /* If the task was loaded by exec(), exec_image holds the per-PT_LOAD
   * PMM allocations so sched_reap can free them all. For tasks running
   * the kernel-shared .text image (every task created via
   * sched_create_task), region_count is 0. */
  elf_image_t exec_image;
  char name[16];
  struct fd_table *fds;
  struct task *next;
  /* Demand-paged stack growth tracking. Each entry is the physical base
   * of a 4 KiB page that was lazily mapped into the user stack region by
   * sched_try_grow_stack(); sched_reap() walks the array to pmm_free them.
   * Placed at the end of task_t so the existing TASK_TTBR0=40 offset used
   * by switch.S stays put. */
  uintptr_t stack_grown_phys[USER_STACK_GROWN_MAX];
  /* User VA each entry above is mapped at. Demand growth fills the zone in
   * whatever order the faults arrive — not top-down — so the VA cannot be
   * recovered from the array index and has to be recorded. sched_fork needs
   * it to re-map the child's copies at the same addresses. */
  uintptr_t stack_grown_va[USER_STACK_GROWN_MAX];
  uint16_t  stack_grown_count;

  /* User address-space regions created by mmap(). Embedded by value so the
   * list's lifetime is exactly the task's — an allocation that could fail
   * here would mean mmap() silently losing a region. Placed after the
   * stack-growth fields to keep the fixed offsets switch.S relies on. */
  vma_set_t vmas;

  /* Program break for the brk()/sbrk() heap. brk_base is a fixed arena base
   * reserved per task so growth is always contiguous; brk_cur is the current
   * break. brk regions are demand-paged like any other anonymous VMA, so the
   * reservation costs no physical memory until touched. */
  uint64_t brk_base;
  uint64_t brk_cur;

  /* Physical page backing this task's TLS block at USER_TLS_BASE. TPIDR_EL0
   * holds the user VA of that page; errno lives at TLS_ERRNO_OFFSET within
   * it. Tracked by physical address because that's what reap needs to free
   * it, and because the kernel writes errno through PHYS_TO_VIRT so the store
   * works regardless of which task's TTBR0 happens to be loaded. */
  uintptr_t tls_phys;
} task_t;

// switch.S
extern void context_switch(task_t *prev, task_t *next);

void sched_init(void);
int sched_create_task(const char *name, task_entry_t entry);

/* fork() implementation — deep-copies the calling task into a new task and
 * returns the child pid (called from SYS_FORK; the trap_frame_t pointer is
 * the parent's frame on its kstack). The child's frame is prepared so that
 * on its first schedule, fork_return ereturns to the same instruction the
 * parent will return to, with x0 = 0. */
struct trap_frame;
int sched_fork(task_t *parent, struct trap_frame *frame);
extern void fork_return(void);


/* Create an EL1 kernel-mode task. Unlike sched_create_task, no user_l0
 * is allocated and TTBR0 is left at 0 (so context_switch skips the
 * per-task user mapping swap). The entry function runs at EL1 with the
 * kernel's full privileges and is expected to call task_exit() or fall
 * off the end (kernel_task_trampoline calls task_exit if it returns). */
int sched_create_kernel_task(const char *name, task_entry_t entry);
void schedule(void);
void yield(void);
void task_exit(void);

/* Kill the task with the given pid. Marks it DEAD, unlinks it from the
 * run queue, and pushes it onto the reaper list. If pid refers to the
 * caller, falls through to task_exit(). Returns 0 on success, -1 if the
 * pid is not found, already dead, or refers to the idle task (pid 0). */
int sched_kill_task(uint64_t pid);
void sleep_ms(uint64_t ms);
void sched_wake_sleepers(void);
void sched_reap(void);
task_t *sched_current(void);

/* Iteration support for /proc/tasks. The run queue is circular and
 * always non-empty (idle is always present); walk t->next until you
 * loop back to the returned head. */
task_t *sched_first_task(void);
const char *task_state_name(task_state_t s);

/* Allocate a fresh ASID for a new user address space.
 *
 * Returns a 16-bit value in [1, 65535]. ASID 0 is reserved for the kernel
 * (idle / sched_create_kernel_task whose ttbr0 stays 0).
 *
 * On wraparound (every 65535 task creations) does a global TLB flush and
 * resets the counter so the recycled ASIDs are guaranteed not to alias
 * stale TLB entries. With the current hobby-kernel workloads we never
 * observe a wrap. */
uint16_t sched_asid_alloc(void);


/* Demand-page handler for the user stack. Called from the EL0 data-abort
 * path BEFORE the fatal kill. If `far` lies in the growth zone
 * [USER_STACK_TOP - USER_STACK_PAGES_MAX * PAGE, USER_STACK_TOP -
 * USER_STACK_PAGES * PAGE) and the task hasn't exhausted USER_STACK_GROWN_MAX,
 * allocates a fresh PMM page, zeros it, and maps it into t->ttbr0 at the
 * page containing far. Returns 1 on success (caller should resume the user
 * task), 0 on failure (caller should fall through to the fault dump). */
int sched_try_grow_stack(task_t *t, uint64_t far);

/* Demand-fault handler for mmap()ed regions. Called from the EL0 data-abort
 * path before the fatal kill.
 *
 * If `far` lands inside a VMA on `t` and the requested access is permitted by
 * that VMA's prot bits, allocate + zero a PMM page, install it in t->ttbr0,
 * invalidate the TLB, and return 1 so the caller resumes the task. Returns 0
 * if the address is outside every VMA or the access violates its protection —
 * in which case the caller must treat it as a genuine fault.
 *
 * `is_write` selects write-vs-read (from the ESR WnR bit) so a write to a
 * PROT_READ mapping still faults instead of being silently satisfied.
 * `is_exec` is set for instruction aborts, which need UXN clear. */
int sched_try_handle_vma_fault(task_t *t, uint64_t far, int is_write,
                               int is_exec);


#endif
