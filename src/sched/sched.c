#include "sched.h"
#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h" // IWYU pragma: keep
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
  // Allocate task struct from heap
  task_t *t = (task_t *)kmalloc(sizeof(task_t));
  if (!t) {
    uart_errorln("[SCHED] Failed to allocate task struct");
    return -1;
  }
  memset(t, 0, sizeof(task_t));

  // Allocate stack pages
  uintptr_t stack_phys = pmm_allocate_pages(TASK_STACK_PAGES);
  if (!stack_phys) {
    uart_errorln("[SCHED] Failed to allocate task stack");
    kfree(t);
    return -1;
  }

  uintptr_t stack_va = PHYS_TO_VIRT(stack_phys);
  uint64_t stack_size = TASK_STACK_PAGES * PAGE_SIZE;
  uintptr_t stack_top = stack_va + stack_size;

  memset((void *)stack_va, 0, stack_size);

  // Set up initial stack frame for context_switch:
  //   [sp + 0]  x19  ← real entry point (trampoline reads this)
  //   [sp + 8]  x20 .. [sp + 72] x28  = 0
  //   [sp + 80] x29  = 0
  //   [sp + 88] x30  ← task_trampoline (context_switch ret's here)
  uint64_t *frame = (uint64_t *)(stack_top - 96);
  frame[0] = (uint64_t)entry;            // x19
  frame[11] = (uint64_t)task_trampoline; // x30 (lr)

  t->sp = (uint64_t)frame;
  t->pid = next_pid++;
  t->state = TASK_READY;
  t->stack_phys = stack_phys;
  copy_name(t->name, name);

  // Tail insertion: append at end of circular queue so tasks run in creation order
  task_t *tail = current;
  while (tail->next != current) {
    tail = tail->next;
  }
  tail->next = t;
  t->next = current;

  uart_printf("[SCHED] Created task %d '%s' | stack: %x - %x\n", t->pid, name,
              stack_va, stack_top);

  return (int)t->pid;
}

void schedule(void) {
  task_t *prev = current;
  task_t *next = prev->next;

  while (next != prev) {
    if (next->state == TASK_READY) {
      break;
    }
    next = next->next;
  }

  if (next == prev) {
    return;
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

  // Switch away permanently, the current task is still on the dying task's stack,
  // so it cannot freed here. The idle loop calls sched_reap().
  schedule();
}

void sched_reap(void) {
  while (dead_list) {
    task_t *dead = dead_list;
    dead_list = dead->next;

    uart_printf("[SCHED] Reaping task %d '%s'\n", dead->pid, dead->name);

    if (dead->stack_phys) {
      pmm_free_pages(dead->stack_phys, TASK_STACK_PAGES);
    }
    kfree(dead);
  }
}
