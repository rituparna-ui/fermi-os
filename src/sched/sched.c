#include "sched.h"
#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "timer/timer.h"
#include "uart/uart.h"

// switch.S: unmasks IRQs, calls x19, then calls task_exit
extern void task_trampoline(void);

// Idle task is statically allocated, always exists
static task_t idle_task;
static task_t *current = &idle_task;
static uint64_t next_pid = 0;

// Dead tasks scheduled for cleanup (singly-linked list)
static task_t *dead_list = (void *)0;

static void copy_name(char *dst, const char *src) {
  for (int i = 0; src[i] && i < 15; i++) {
    dst[i] = src[i];
  }
}

void sched_init(void) {
  uart_println("[SCHED] Initializing scheduler");

  memset(&idle_task, 0, sizeof(task_t));
  idle_task.pid = next_pid++;
  idle_task.state = TASK_RUNNING;
  idle_task.stack_phys = 0;    // kernel stack, not PMM-managed
  idle_task.next = &idle_task; // circular: points to itself
  copy_name(idle_task.name, "idle");

  current = &idle_task;

  uart_println("[SCHED] Initialized! Idle task registered");
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

  // Map the 2MB block containing the entry function into user space
  // Entry is a kernel VA — convert to physical, find 2MB-aligned base
  uint64_t entry_pa = VIRT_TO_PHYS((uint64_t)entry);
  uint64_t entry_block = entry_pa & ~(_2MB - 1);
  uint64_t entry_offset = entry_pa - entry_block;

  // EL0 can read+execute, kernel cannot execute (PXN)
  // AP[7:6]=01 means EL0 RW, EL1 RW, but we want EL0 RO+exec
  // AP=11 → EL0 RO, EL1 RO. UXN=0 allows EL0 execute.
  uint64_t text_flags = PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_PXN;
  mmu_map_user_page(user_l0, USER_TEXT_BASE, entry_block, text_flags);

  uint64_t user_entry = USER_TEXT_BASE + entry_offset;

  // User stack
  uintptr_t ustack_phys = pmm_allocate_pages(USER_STACK_PAGES);
  if (!ustack_phys) {
    uart_errorln("[SCHED] Failed to allocate user stack");
    pmm_free_pages(kstack_phys, TASK_STACK_PAGES);
    kfree(t);
    return -1;
  }
  uintptr_t ustack_va_kern = PHYS_TO_VIRT(ustack_phys);
  memset((void *)ustack_va_kern, 0, USER_STACK_PAGES * PAGE_SIZE);

  // Map user stack into user address space at USER_STACK_TOP - size
  uint64_t ustack_user_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  // Each page is within one 2MB block since stack is small and aligned
  uint64_t stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  mmu_map_user_page(user_l0, ustack_user_base, ustack_phys, stack_flags);

  // Set up initial kernel stack frame for context_switch
  // task_trampoline will eret to EL0 using x19=user_entry, x20=user_sp
  uint64_t *frame = (uint64_t *)(kstack_top - 96);
  frame[0] = user_entry;     // x19 — user entry point
  frame[1] = USER_STACK_TOP; // x20 — user SP
  frame[11] =
      PHYS_TO_VIRT((uint64_t)task_trampoline); // x30 (lr) — must be TTBR1 VA

  t->sp = (uint64_t)frame;
  t->pid = next_pid++;
  t->state = TASK_READY;
  t->stack_phys = kstack_phys;
  t->ttbr0 = (uint64_t)user_l0;
  t->user_sp = USER_STACK_TOP;
  t->kstack_top = kstack_top;
  t->ustack_phys = ustack_phys;
  copy_name(t->name, name);

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

    // Free per-task TTBR0 page table pages (L0, L1, L2)
    if (dead->ttbr0) {
      mmu_free_user_tables((uint64_t *)dead->ttbr0);
    }

    kfree(dead);
  }
}

task_t *sched_current(void) { return current; }
