# Scheduler Subsystem Porting Specification

## Overview

The Fermi OS scheduler is a **round-robin preemptive task scheduler** for aarch64 running at EL1 (kernel mode). It implements:

- **Per-task kernel stacks** (16 KiB = 4 pages) used during exception handling and context switches
- **Per-task user address spaces** with demand-paged stack growth (up to 256 KiB)
- **Per-task TTBR0 + ASID** isolation so user TLB entries are automatically tagged and don't collide across tasks
- **Circular run queue** with task states: READY, RUNNING, SLEEPING, DEAD
- **Context switch** via `context_switch()` assembly that saves/restores callee-saved registers (x19–x30, d8–d15)
- **Fork/exec** support for spawning new user tasks
- **Kernel-mode tasks** (EL1) that share TTBR1 (kernel page tables)
- **Demand-paged stack growth** callback from the EL0 data-abort handler
- **Task reaping** (cleanup of dead tasks' physical memory)
- **ASID allocation and wraparound** (65535-task budget with global TLB flush on wrap)

---

## Constants & Magic Numbers

```c
/* Kernel stack layout */
#define TASK_STACK_PAGES 4               // 16 KiB per task kernel stack
#define TASK_STACK_BYTES (16 * 1024)     // 16384 = TASK_STACK_PAGES * 4096

/* User stack (mapped into TTBR0) */
#define USER_STACK_PAGES 4               // 16 KiB initial eager allocation
#define USER_STACK_BYTES (16 * 1024)     // 16384 = USER_STACK_PAGES * 4096
#define USER_STACK_PAGES_MAX 64          // Hard cap for demand-paged growth
#define USER_STACK_GROWN_MAX (64 - 4)    // 60 extra pages possible

/* Trap frame size (from vector.S) */
#define FRAME_SIZE 688                   // GPRs + sysregs + SP_EL0 + FP (q0-q7, q16-q31, FPSR, FPCR)

/* Context switch frame (callee-saved) */
#define CONTEXT_SWITCH_FRAME 160         // 12 GPRs (x19-x30) + 8 SIMD (d8-d15)

/* TTBR0 ASID encoding (ARMv8) */
#define TTBR_ASID_SHIFT 48               // TTBR[63:48] = ASID
#define TTBR_ASID_MAX 65535              // 16-bit ASID space [1..65535] (0 reserved)
#define TTBR_BADDR_MASK 0x0000FFFFFFFFFFFFULL  // [47:0] = page-table base
```

---

## Task Structure Layout

```c
typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_SLEEPING = 2,
  TASK_DEAD = 3
} task_state_t;

typedef struct task {
  /* Offset 0: kernel SP (saved by context_switch) */
  uint64_t sp;
  
  /* Offset 8 */
  uint64_t pid;
  
  /* Offset 16 */
  task_state_t state;
  
  /* Offset 24 */
  uint64_t sleep_until;
  
  /* Offset 32: physical address of kernel stack for this task */
  uintptr_t stack_phys;
  
  /* Offset 40: packed TTBR0 (ASID in [63:48], L0 base in [47:0]) */
  uint64_t ttbr0;
  
  /* Offset 48: user SP_EL0 when task is running at EL0 */
  uint64_t user_sp;
  
  /* Offset 56: top of kernel stack (kstack_va + TASK_STACK_BYTES) */
  uintptr_t kstack_top;
  
  /* Offset 64: physical base of user stack (for cleanup) */
  uintptr_t ustack_phys;
  
  /* Offset 72: exec_image_t (PT_LOAD regions if task was loaded by exec()) */
  elf_image_t exec_image;
  
  /* Offset ~100: 16-byte task name */
  char name[16];
  
  /* Offset ~116: per-task fd table (or NULL for kernel tasks) */
  struct fd_table *fds;
  
  /* Offset ~124: next task in circular run queue */
  struct task *next;
  
  /* Offset ~132: demand-paged stack pages (up to 60 extra pages) */
  uintptr_t stack_grown_phys[USER_STACK_GROWN_MAX];  /* 60 * sizeof(void*) */
  
  /* Offset ~612: count of demand-paged pages allocated */
  uint16_t stack_grown_count;
  
  /* Padding to align task_t size */
} task_t;
```

**Critical offset for assembly code:**
- `TASK_SP = 0` — context_switch uses `str x2, [x0, #0]` to save sp
- `TASK_TTBR0 = 40` — context_switch uses `ldr x3, [x1, #40]` to load next task's TTBR0

---

## Task States & Transitions

```
TASK_READY ──→ TASK_RUNNING
                      ↓
                schedule() or yield()
                      ↓
                   (rescheduled)
                      ↓
                TASK_READY or TASK_SLEEPING

TASK_RUNNING ──→ task_exit() or sched_kill_task()
                      ↓
                   TASK_DEAD ──→ sched_reap() ──→ freed
```

- **TASK_READY**: Candidate for scheduling; not currently executing
- **TASK_RUNNING**: Currently executing on the CPU
- **TASK_SLEEPING**: Blocked waiting for timer to reach `sleep_until`; woken by `sched_wake_sleepers()`
- **TASK_DEAD**: Marked for cleanup; unlinked from run queue, pushed to `dead_list`

---

## Global State

```c
/* Current task (pointer into the run queue) */
static task_t *current;

/* Idle task: statically allocated, always exists, lowest priority */
static task_t idle_task;

/* Dead tasks pending cleanup (singly-linked list) */
static task_t *dead_list;

/* PID counter */
static uint64_t next_pid;

/* ASID allocator: [1..65535], wraps with global TLB flush */
static uint16_t next_asid;
```

The run queue is **circular**: idle_task → task1 → task2 → ... → taskN → idle_task

---

## Public API

### Initialization & Creation

#### `void sched_init(void)`

**Invoked:** During kernel_main() before any tasks are created.

**Behavior:**
- Initializes the idle task (statically allocated)
- Sets `idle_task.state = TASK_READY` (not RUNNING, so schedule() can pick it as fallback)
- Sets `idle_task.pid = 0`, `idle_task.stack_phys = 0` (kernel-managed)
- Sets `idle_task.next = &idle_task` (circular, points to itself initially)
- Name: "idle"

**No allocation, no error path.**

---

#### `int sched_create_task(const char *name, task_entry_t entry)`

**Invoked:** For EL0 user tasks (e.g., task_shell, task_a, task_b).

**Returns:** 
- `>= 0`: pid of the new task
- `-1`: allocation failure (kernel stack, user page tables, user stack, or heap)

**Parameters:**
- `name`: Max 15 characters (copied into `task_t.name[16]`); NUL-terminated
- `entry`: Function pointer (EL0 user-mode entry point, VA after mapping)

**Behavior:**

1. **Allocate task_t struct** from kernel heap
2. **Allocate kernel stack** (4 pages = 16 KiB) via `pmm_allocate_pages(TASK_STACK_PAGES)`
3. **Create user page tables** via `mmu_create_user_tables()` (returns L0 base)
4. **Map .text + .rodata** (read-only, EL0 RX, kernel PXN) at `USER_TEXT_BASE`
   - Range: `[__text_start, __user_text_end)`
   - Flags: `PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_PXN`
   - **Critical:** Map entire range so PC-relative addressing (ADRP) can reach helpers placed before entry
5. **Allocate user stack** (4 pages = 16 KiB) at top of user VA space
   - VA: `USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE` to `USER_STACK_TOP`
   - Flags: `PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN`
6. **Set up kernel-stack trap frame** (160 bytes for context_switch):
   - Frame is at `(kstack_top - 160)`
   - `frame[0] (x19)` ← user entry point (VA in user address space)
   - `frame[1] (x20)` ← USER_STACK_TOP (user SP)
   - `frame[11] (x30/lr)` ← `task_trampoline` (TTBR1 VA, PC-independent)
   - SIMD regs d8–d15 left zero
7. **Pack TTBR0** with fresh ASID: `ttbr0 = ttbr_pack(user_l0, sched_asid_alloc())`
8. **Create fd table** (stdin/stdout/stderr → `/dev/console`) via `fd_table_create()`
9. **Insert into run queue**: Find tail of circular queue, insert before current
10. **Set state** ← TASK_READY, not RUNNING

**Failure cleanup:** Unwind allocations in reverse order; free heap, PMM pages, user_l0

---

#### `int sched_create_kernel_task(const char *name, task_entry_t entry)`

**Invoked:** For EL1 kernel-mode tasks (e.g., netd).

**Returns:** 
- `>= 0`: pid of the new kernel task
- `-1`: allocation failure

**Parameters:** Same as `sched_create_task`

**Behavior:** Mirrors `sched_create_task` but:
- No user page tables (ttbr0 stays 0)
- No user stack
- No fd table
- Kernel-stack trap frame uses `kernel_task_trampoline` instead of `task_trampoline`
- Context switch skips TTBR0 swap when `ttbr0 == 0`

---

#### `int sched_fork(task_t *parent, struct trap_frame *frame)`

**Invoked:** From SYS_FORK syscall handler; parent task supplies its trap frame.

**Returns:** 
- Child's pid (to parent)
- `0` (to child, after first context_switch)
- `-1` on allocation failure

**Parameters:**
- `parent`: Calling task
- `frame`: Parent's trap frame (288 bytes) on parent's kstack during SVC

**Behavior:**

1. **Allocate child task_t and kernel stack** (same as create_task)
2. **Create fresh user page tables** for child
3. **Re-map .text + .rodata** with same physical pages (read-only, safe)
4. **Allocate fresh user stack** and **copy parent's stack contents** byte-for-byte
5. **Lay out child's kstack**:
   - Copy parent's trap frame (688 bytes) verbatim to `(kstack_top - 688)`
   - Clobber x0 ← 0 (distinguishes child from parent in syscall return)
   - Lay out context_switch frame (160 bytes) below trap frame at `(kstack_top - 848)`
   - `context_switch_frame[0] (x19)` ← 0 (unused)
   - `context_switch_frame[1] (x20)` ← 0 (unused)
   - `context_switch_frame[11] (x30)` ← `fork_return` (returns to trap frame, ereturns to EL0)
6. **Allocate fresh ASID** for child (separate from parent)
7. **Set fd table** (fresh stdin/stdout/stderr, not inherited from parent)
8. **Append "+f" to name** if space permits (cosmetic: "task+f")
9. **Insert into run queue** (with IRQs masked)
10. **Return child's pid** to parent (parent continues past SVC); child wakes when scheduled

**First execution:** Child's context_switch pops 160-byte frame and rets to `fork_return`, which restores trap frame and ereturns to EL0 at same PC as parent's SVC.

---

### Scheduling & Preemption

#### `void schedule(void)`

**Invoked:** 
- Periodically by timer tick interrupt (via timer ISR)
- Voluntarily by `yield()` or `task_exit()` or `sleep_ms()`

**Behavior (round-robin preemption):**

1. **Call `sched_reap()`** to clean up dead tasks
2. **Scan run queue** starting from `current->next`:
   - Find first task with `state == TASK_READY`
   - Skip idle task if any other ready task exists
   - If no ready task found, use idle as fallback
3. **If current task is READY** and no other task is READY:
   - No context switch; current keeps its timeslice
   - Return without calling `context_switch()`
4. **Transition states**:
   - If `prev->state == TASK_RUNNING` → set to TASK_READY
   - If `prev->state == TASK_DEAD` → unlink from run queue, push to dead_list
5. **Set next task** state ← TASK_RUNNING
6. **Call `context_switch(prev, next)`** (assembly, saves/restores registers + TTBR0)

---

#### `void yield(void)`

**Behavior:** Calls `schedule()` directly. Voluntary preemption point.

---

#### `void sleep_ms(uint64_t ms)`

**Invoked:** From SYS_SLEEP syscall or kernel tasks via `sleep_ms()`.

**Behavior:**
1. Convert ms to ticks: `ticks_needed = ms / TIMER_INTERVAL_MS` (clamped to ≥1)
2. Set `current->sleep_until = timer_get_ticks() + ticks_needed`
3. Set `current->state = TASK_SLEEPING`
4. Call `schedule()` (will skip this task until timer wakes it)

---

#### `void sched_wake_sleepers(void)`

**Invoked:** Periodically by timer ISR after each tick.

**Behavior:**
- Walk the run queue
- For each SLEEPING task, if `timer_get_ticks() >= task->sleep_until`, set state ← TASK_READY

---

### Task Lifecycle

#### `void task_exit(void)`

**Invoked:** 
- From `task_trampoline` if user entry returns (shouldn't happen normally)
- Explicitly from SYS_EXIT or user `sys_exit()`
- From `kernel_task_trampoline` if kernel entry returns
- From `sched_kill_task()` when killing a sleeping/ready task

**Behavior:**
1. Log task exit
2. Set `current->state = TASK_DEAD`
3. Call `schedule()` (switches away permanently; current task dies)

**After task_exit returns, never executes again.** Cleanup deferred to `sched_reap()`.

---

#### `int sched_kill_task(uint64_t pid)`

**Returns:** 
- `0` on success
- `-1` if pid not found, already dead, or pid == 0 (idle)

**Behavior:**
1. **Refuse to kill idle** (pid 0)
2. **Search run queue** for matching pid
3. **If current task:** Fall through to `task_exit()` (self-exit path)
4. **If other task** (mask IRQs during queue mutation):
   - Unlink from run queue
   - Push to dead_list
   - Set state ← TASK_DEAD
5. Return 0 on success, -1 on failure

---

### Cleanup & Reaping

#### `void sched_reap(void)`

**Invoked:** At start of `schedule()` and idle loop.

**Behavior:** Process entire `dead_list`:

For each dead task:
1. **Free kernel stack** via `pmm_free_pages(stack_phys, TASK_STACK_PAGES)`
2. **Free user stack** via `pmm_free_pages(ustack_phys, USER_STACK_PAGES)`
3. **Free exec_image regions** (if task was loaded by exec()):
   - For each PT_LOAD region in `exec_image.regions[]`, pmm_free_pages the physical pages
4. **Free demand-paged stack pages**:
   - Walk `stack_grown_phys[]` array, pmm_free_page() each
5. **Invalidate TLB entries** for this task's ASID (if ttbr0 != 0):
   - Issue `tlbi aside1` with ASID in top 16 bits to flush all VA×ASID pairs
   - Before freeing page tables (so stale entries don't point at freed pages after ASID recycle)
6. **Free user page tables** via `mmu_free_user_tables(user_l0)`
7. **Free fd table** via `fd_table_destroy(fds)`
8. **Free task_t struct** via `kfree(t)`

---

### Query API

#### `task_t *sched_current(void)`

**Returns:** Pointer to currently-executing task.

---

#### `task_t *sched_first_task(void)`

**Returns:** Pointer to idle task (head of circular run queue).

**Usage:** For iteration:
```c
task_t *head = sched_first_task();
task_t *t = head;
do {
  /* process t */
  t = t->next;
} while (t != head);
```

---

#### `const char *task_state_name(task_state_t s)`

**Returns:** String name ("READY", "RUNNING", "SLEEPING", "DEAD", or "?")

---

### ASID Management

#### `uint16_t sched_asid_alloc(void)`

**Returns:** Fresh 16-bit ASID in range [1, 65535].

**Behavior:**
1. Return `next_asid++`
2. If wrapped to 0, flush entire TLB via `tlbi vmalle1` and reset counter to 1

**Critical behavior:** 
- ASID 0 is reserved (kernel / idle task with ttbr0 == 0)
- Each unique user task gets a unique ASID
- On wraparound (after 65535 allocations), global TLB flush ensures recycled ASIDs can't alias stale entries

---

### Demand-Paged Stack Growth

#### `int sched_try_grow_stack(task_t *t, uint64_t far)`

**Invoked:** From EL0 data-abort handler when user task faults.

**Returns:** 
- `1` on success (page mapped; user should retry the faulting instruction)
- `0` on failure (fault outside growth zone, cap exhausted, or PMM empty)

**Parameters:**
- `t`: Task structure of faulting task (usually `sched_current()`)
- `far`: Faulting address from FAR_EL1

**Behavior:**

1. **Validate task** (return 0 if task is NULL or ttbr0 == 0)
2. **Check bounds**:
   - Faulting VA must be in `[USER_STACK_TOP - USER_STACK_PAGES_MAX * PAGE_SIZE, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)`
   - Return 0 if outside this growth zone
3. **Check cap** (return 0 if `task->stack_grown_count >= USER_STACK_GROWN_MAX`)
4. **Allocate & zero page**:
   - `pa = pmm_allocate_page()` (return 0 if empty)
   - `memset(PHYS_TO_VIRT(pa), 0, PAGE_SIZE)`
5. **Map into user address space**:
   - Align FAR to page boundary: `va = far & ~(PAGE_SIZE - 1)`
   - Flags: `PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN`
   - Call `mmu_map_user_range(user_l0, va, pa, 1, flags)`
6. **Invalidate TLB**:
   - Extract ASID from `ttbr0`: `asid = ttbr_asid(t->ttbr0)`
   - Construct TLB invalidate arg: `arg = (asid << TTBR_ASID_SHIFT) | (va >> 12)`
   - Issue `dsb ish`, then `tlbi vae1, arg`, then `dsb ish`, then `isb`
7. **Track allocation**:
   - Store `pa` in `task->stack_grown_phys[stack_grown_count++]`
8. **Return 1** (success)

---

## Assembly Routines (Must Stay in Asm)

### `void context_switch(task_t *prev, task_t *next)` (switch.S)

**Parameters:**
- x0 = prev task pointer
- x1 = next task pointer

**Registers used (not preserved):**
- x0, x1, x2, x3

**Behavior:**

1. **Save prev's callee-saved regs** onto prev's stack (via sp):
   - Allocate 160 bytes: `sub sp, sp, #160`
   - Save GPRs: x19–x30 at [sp+0], [sp+8], ..., [sp+88]
   - Save SIMD: d8–d15 at [sp+96], [sp+104], ..., [sp+152]

2. **Save prev's sp**:
   - `mov x2, sp`
   - `str x2, [x0, #TASK_SP]` (offset 0 in task_t)

3. **Load next's sp**:
   - `ldr x2, [x1, #TASK_SP]`
   - `mov sp, x2`

4. **Swap TTBR0** (if next is a user task):
   - `ldr x3, [x1, #TASK_TTBR0]` (offset 40 in task_t)
   - If `x3 == 0`, skip (kernel task); else:
     - `msr ttbr0_el1, x3`
     - `isb` (sequence TTBR0 write before next instruction)

5. **Restore next's callee-saved regs** from next's stack:
   - Restore SIMD: d8–d15 from [sp+96], ..., [sp+152]
   - Restore GPRs: x19–x30 from [sp+0], ..., [sp+88]
   - Deallocate: `add sp, sp, #160`

6. **Return** (`ret`) into next task's saved x30 (which could be entry point, fork_return, etc.)

**Why assembly:** 
- Direct sp manipulation (AAPCS allows no C-level SP caching)
- TTBR0_EL1 MSR (sysreg access) via inline asm would clobber more registers
- Callee-saved frame layout must match vector.S exception frame exactly
- Critical path: must be as fast as possible

---

### `void task_trampoline()` (switch.S)

**Entered:** First time a user task runs (ret from context_switch into this address)

**Registers at entry:**
- x19 = user entry point (VA in user address space)
- x20 = user SP_EL0
- IRQs masked (we were switched-in from interrupt context)

**Behavior:**

1. **Unmask IRQs**: `msr daifclr, #2` (clear bit 1 of DAIF to unmask IRQs)
2. **Set up EL0 registers**:
   - `msr sp_el0, x20` (user stack pointer)
   - `msr elr_el1, x19` (user entry point; will be PC after eret)
   - `mov x0, #0` (zero for SPSR_EL1)
   - `msr spsr_el1, x0` (SPSR.M[3:0] = 0 = EL0t)
3. **Drop to EL0**: `eret` (exception return to EL0)

**User task then executes from x19 with stack at SP_EL0.**

---

### `void kernel_task_trampoline()` (switch.S)

**Entered:** First time a kernel task runs (ret from context_switch into this address)

**Registers at entry:**
- x19 = entry function pointer (EL1 kernel address)
- IRQs masked

**Behavior:**

1. **Unmask IRQs**: `msr daifclr, #2`
2. **Call entry function**: `blr x19` (branch-and-link to entry)
3. **If entry returns**, fall through to `task_exit()`
4. **If task_exit returns** (shouldn't), spin loop

---

### `void fork_return()` (switch.S)

**Entered:** First time a forked child runs (ret from context_switch into this address)

**Stack at entry:** Points to child's trap frame (288 bytes, laid out by sched_fork)

**Behavior:**

1. **Restore ELR_EL1 and SPSR_EL1** from trap frame offsets 248 and 256
2. **Restore SP_EL0** (if SPSR.M[3:0] == 0, i.e., returning to EL0):
   - Load SP_EL0 from offset 280
   - Otherwise skip (kernel mode)
3. **Restore all GPRs** from trap frame:
   - x0–x30 from offsets 0, 8, 16, ..., 240
4. **Deallocate trap frame**: `add sp, sp, #288`
5. **Return to EL0 (or EL1)** via `eret`

**Child then resumes inside the same SVC that forked it, with x0 = 0 (identifying it as the child).**

---

## Boot/Usage Ordering

1. **Early:** `sched_init()` called in kernel_main() after MMU, heap, exceptions are up
2. **Create tasks:** `sched_create_task()` and `sched_create_kernel_task()` to populate run queue
3. **Start timer:** `timer_start()` begins periodic ticks
4. **Idle loop:** kernel_main() spins in `while(1) { wfi; }`, waiting for interrupts
5. **Timer ISR:** Calls `sched_wake_sleepers()` then `schedule()`
6. **Data-abort ISR:** On EL0 fault, calls `sched_try_grow_stack(sched_current(), far)` before kill
7. **SYS_* handlers:** Invoke `sched_fork()`, `sched_kill_task()`, `sleep_ms()`, `task_exit()`, etc.
8. **Idle task:** Always present as fallback; runs `while(1) { wfi; }` when nothing else is ready

---

## Rust Module Design (Implementation Strategy)

### Module Structure

```
src/sched/
  ├── mod.rs           (initialization, public API wrappers)
  ├── task.rs          (Task struct, task_state enum, task_entry_t function pointer type)
  ├── queue.rs         (circular queue insertion/removal, iteration)
  ├── allocator.rs     (ASID allocation & wraparound, next_pid counter)
  ├── reaper.rs        (dead_list, sched_reap cleanup)
  ├── sleep.rs         (sleep_until tracking, sched_wake_sleepers)
  ├── switch.rs        (unsafe { asm! } wrappers around context_switch asm)
  └── switch.S         (assembly: context_switch, task_trampoline, kernel_task_trampoline, fork_return)
```

### Key Ownership & Locking

- **`current: &'static mut task_t`**: Single mutable reference to current task (only swapped under timer IRQ)
- **`idle_task: task_t`**: Static, never freed
- **`dead_list: Option<&'static mut task_t>`**: Singly-linked chain, only accessed from schedule() / sched_reap()
- **`run_queue`**: Circular linked list; not directly exposed; accessed via `task_t.next` pointers

**Synchronization:**
- **No lock on run_queue.** Mutations happen at:
  - Task creation: append to queue tail (IRQs masked if called from SVC)
  - Task kill: unlink & move to dead_list (IRQs masked)
  - Schedule: update current & state (always from IRQ context or syscall)
- **IRQ masking:** When syscalls or non-IRQ code mutates the queue, manually mask/unmask DAIF[2] (IRQ bit)

### Core Types

```rust
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TaskState {
    Ready = 0,
    Running = 1,
    Sleeping = 2,
    Dead = 3,
}

pub type TaskEntry = extern "C" fn() -> !;

pub struct Task {
    pub sp: u64,                    // Offset 0: kernel SP
    pub pid: u64,
    pub state: TaskState,
    pub sleep_until: u64,
    pub stack_phys: usize,
    pub ttbr0: u64,                 // Offset 40 (MUST match asm)
    pub user_sp: u64,
    pub kstack_top: usize,
    pub ustack_phys: usize,
    pub exec_image: ExecImage,      // PT_LOAD regions
    pub name: [u8; 16],
    pub fds: Option<&'static mut FdTable>,
    pub next: Option<&'static mut Task>,
    pub stack_grown_phys: [usize; USER_STACK_GROWN_MAX],
    pub stack_grown_count: u16,
}

extern "C" {
    pub fn context_switch(prev: &mut Task, next: &mut Task);
    pub fn task_trampoline() -> !;
    pub fn kernel_task_trampoline() -> !;
    pub fn fork_return() -> !;
}
```

### Public API (Rust Wrappers)

```rust
pub fn sched_init() -> Result<(), Error>;

pub fn sched_create_task(name: &str, entry: TaskEntry) -> Result<u64, Error>;

pub fn sched_create_kernel_task(name: &str, entry: TaskEntry) -> Result<u64, Error>;

pub fn sched_fork(parent: &mut Task, frame: &TrapFrame) -> Result<u64, Error>;

pub fn schedule();

pub fn yield_task();

pub fn task_exit() -> !;

pub fn sched_kill_task(pid: u64) -> Result<(), Error>;

pub fn sleep_ms(ms: u64);

pub fn sched_wake_sleepers();

pub fn sched_reap();

pub fn sched_current() -> &'static mut Task;

pub fn sched_first_task() -> &'static Task;

pub fn task_state_name(s: TaskState) -> &'static str;

pub fn sched_asid_alloc() -> u16;

pub fn sched_try_grow_stack(t: &mut Task, far: u64) -> i32;
```

### Critical Unsafe Blocks

1. **Pointer dereferencing** for linked-list traversal (run_queue, dead_list)
2. **Raw sp manipulation** in context_switch inline asm
3. **Sysreg access** (MSR TTBR0_EL1, DAIF, tlbi, dsb, isb)
4. **Inline asm constraints** for context_switch frame layout

### Core/Alloc Features Used

- `#![no_std]` (no libstd)
- `alloc::vec::Vec` or custom linked-list for task tracking
- `core::cell::UnsafeCell` for interior mutability (current, dead_list)
- `core::mem::offset_of!` to verify task_t field offsets match asm

---

## Hardware Details & Magic Constants

### ARMv8 Registers & Fields

```
DAIF [7:0]:
  [7] D  Debug exception mask
  [6] A  SError exception mask
  [5] I  IRQ exception mask
  [4] F  FIQ exception mask
  (Bit 2 = IRQ mask; daifset #2 masks, daifclr #2 unmasks)

TTBR0_EL1 [63:48]: ASID (when TCR_EL1.AS = 1)
TTBR0_EL1 [47:0]:  Page table base address (page-aligned)

TTBR_ASID_SHIFT = 48
TTBR_BADDR_MASK = 0x0000FFFFFFFFFFFFULL

SPSR_EL1[3:0] (M):
  0000 = EL0t (AArch64 EL0)
  0001 = EL1t (AArch64 EL1 with SP_EL0)
  0101 = EL1h (AArch64 EL1 with SP_EL1)

FAR_EL1: Faulting address register (read on fault)
ESR_EL1: Exception syndrome register (exception reason)
ELR_EL1: Exception link register (faulting PC, used with eret)

PTE flags (Level 3 / page descriptors):
  [0]      Valid
  [1]      Table/Block
  [2-4]    MemAttr (index into MAIR_EL1)
  [5]      nS (non-secure; we set 0)
  [6-7]    AP (access permissions)
  [8-9]    SH (shareability)
  [10]     AF (access flag)
  [11]     nG (non-global; set for user, clear for kernel)
  [53]     PXN (kernel execute never)
  [54]     UXN (user execute never)

PTE_AP_RW_EL0 = (1 << 6)  // EL1 RW, EL0 RW
PTE_AP_RO_EL0 = (3 << 6)  // EL1 RO, EL0 RO
PTE_UXN = (1 << 54)        // EL0 cannot execute
PTE_PXN = (1 << 53)        // EL1 cannot execute
```

### TLB Invalidation

```
tlbi vmalle1      → Invalidate all non-global entries (all VAs, all ASIDs)
tlbi aside1, Xt   → Invalidate by ASID (all VAs for a given ASID in Xt[63:48])
tlbi vae1, Xt     → Invalidate by VA and ASID (Xt[63:48] = ASID, Xt[43:0] = VA[55:12])

Must follow with dsb ish (data sync barrier) then isb (instruction sync).
```

### Constants from Memory/MMU

```
PAGE_SIZE = 4096
PHYS_TO_VIRT(pa) = (pa) + 0xFFFF000000000000
VIRT_TO_PHYS(va) = (va) - 0xFFFF000000000000

USER_TEXT_BASE = ? (defined in linker script / config)
USER_STACK_TOP = ? (defined in linker script / config)
```

---

## Gotchas & Subtle Correctness Issues

### 1. **ASID Wraparound**
- After 65535 task creations, `next_asid` wraps to 0
- Must **immediately** issue `tlbi vmalle1` to flush all TLB entries (global and non-global)
- Without flush, a recycled ASID could alias a stale TLB entry pointing at a page that's been pmm_free'd and reallocated
- Recovery: Reset `next_asid = 1` after flush

### 2. **Context Switch Frame Layout Mismatch**
- Assembly code assumes task_t.sp is at offset 0 and task_t.ttbr0 is at offset 40
- If sched.h changes these offsets, switch.S breaks silently (wrong memory locations saved/loaded)
- **Verify at compile time** or **document the layout in a comment** visible to both files

### 3. **TTBR0 ASID Encoding & TLB Isolation**
- Each user task gets a unique ASID (16-bit value, [1..65535])
- ASID is packed into TTBR0[63:48] by `ttbr_pack()`
- User page-table entries must have `nG=1` (non-global) so they're tagged with ASID
- Kernel mappings have `nG=0` so they're shared across all ASIDs (invisible to non-global lookups)
- Without nG tagging, switching TTBR0 could incorrectly match a stale user entry from another task

### 4. **Callee-Saved Frame Alignment**
- Context-switch frame is 160 bytes (12 GPRs + 8 SIMD)
- Must be 16-byte aligned for SIMD load/store (ldr/str d8 etc.)
- Stack pointer must be 16-byte aligned entering/exiting context_switch
- AAPCS64 compliance required

### 5. **Interrupt Masking for Queue Mutation**
- Syscalls (SVC handler) or non-IRQ code that mutates run_queue must mask IRQs manually
- Otherwise, a timer IRQ could call schedule() mid-mutation, corrupting linked list
- Mask: `__asm__ __volatile__("msr daifset, #2")` (set IRQ mask bit)
- Unmask: `__asm__ __volatile__("msr daif, %0" ::"r"(daif))` (restore original DAIF)

### 6. **Dead Task List vs. Run Queue**
- Dead tasks are **unlinked** from run_queue and **moved** to dead_list (singly-linked)
- sched_reap() processes dead_list and frees everything
- If a task is still in run_queue, it's not dead (state may be READY, RUNNING, or SLEEPING)
- Don't double-free or access dead tasks after they're reaped

### 7. **Demand-Paged Stack Growth**
- Faulting VA must align to page boundary before mapping
- Growth zone is `[USER_STACK_TOP - MAX*PAGE, USER_STACK_TOP - INITIAL*PAGE)`
- Faults within the initial 4 pages (bottom of stack) are permission faults, not growth faults
- ASID extraction from ttbr0 for TLB invalidation: `asid = (ttbr0 >> TTBR_ASID_SHIFT) & 0xFFFF`

### 8. **TLB Invalidation Before Page-Table Freeing**
- When a task dies, its ASID may have stale TLB entries
- Before calling `mmu_free_user_tables()`, issue `tlbi aside1` with the dead task's ASID
- Otherwise, a future ASID recycle could re-use the same value, and stale TLB entries would point at freed pages (security/correctness bug)

### 9. **Fork Stack Copy & Trap Frame**
- Fork deep-copies the parent's user stack (byte-for-byte, including all local variables)
- Fork deep-copies the parent's trap frame but **clobbers x0 ← 0** (to distinguish child)
- On first context_switch into child, fork_return restores the trap frame and eret to EL0
- Child resumes at the same PC as parent but with x0 = 0 (vs. parent seeing child's pid)

### 10. **Idle Task Bootstrap**
- Idle task must be state READY (not RUNNING) so schedule() can pick it as fallback
- Idle has no user page tables (ttbr0 = 0), kernel stack, or fd table
- Idle runs forever in `while(1) { wfi; }`; calling task_exit() on idle is fatal (no fallback)

---

## Linker/Configuration Dependencies

These must be provided by build/linker:
- `__text_start`: Start of kernel .text (read-only, user-mappable)
- `__user_text_end`: End of kernel .text (read-only, user-mappable)
- `USER_TEXT_BASE`: VA where .text is mapped for user tasks
- `USER_STACK_TOP`: Top of user stack VA range
- `TIMER_INTERVAL_MS`: Tick period (probably 10 ms)

---

## Testing Strategy

1. **Single task**: Create one user task, verify schedule() picks it, context_switch works
2. **Multiple tasks**: Create N tasks, verify round-robin preemption (timer tick → schedule → context_switch)
3. **Sleep/wake**: Create task that sleeps, verify sched_wake_sleepers() wakes it on time
4. **Fork**: Create task, fork, verify child and parent both run, x0 differs
5. **Exit/reap**: Create task, kill it, verify sched_reap() cleans up
6. **Stack growth**: Create task, allocate large buffer, verify sched_try_grow_stack() handles faults
7. **ASID wraparound**: Create 65536+ tasks, verify TLB flush on wrap (or mock in test)

