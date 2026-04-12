#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define TASK_STACK_PAGES 4 // 16 KiB per task (kernel stack)
#define USER_STACK_PAGES 4 // 16 KiB user stack

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
  char name[16];
  struct task *next;
} task_t;

// switch.S
extern void context_switch(task_t *prev, task_t *next);

void sched_init(void);
int sched_create_task(const char *name, task_entry_t entry);
void schedule(void);
void yield(void);
void task_exit(void);
void sleep_ms(uint64_t ms);
void sched_wake_sleepers(void);
void sched_reap(void);
task_t *sched_current(void);

#endif
