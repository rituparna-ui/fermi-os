# Design: Preemptive Cross-Core Task Migration (the remaining SMP frontier)

This is the one scheduler variant not yet built. The cooperative path is done
and verified (`feat/smp-migrate`): a task can `pool_yield()`, be requeued, and
resume on the other core. What remains is **involuntary** migration: a timer
interrupt forcibly moves a running task between cores. This document specifies a
correct implementation so it can be executed in a dedicated session.

## Why it is hard: the two-stack race

When core A's timer preempts task `T` and wants another core to be able to run
`T` next, A must (1) save `T`'s context and (2) make `T` available to core B.
If `T` becomes available **before** its context is fully saved, core B can load
`T`'s (stale/partial) context and run it **while A is still executing on T's
stack** → two cores on one stack → silent corruption. The failure is *latent*:
short tests may pass while a rare interleaving corrupts memory.

## Correct mechanism: lazy requeue (Linux `finish_task_switch` pattern)

The task is requeued **only after** its context is fully saved, by the *incoming*
context — never by the switching core before the switch completes.

Per-core state:
```
RUNQ          : SpinLock<deque<*mut Task>>   // shared, IRQ-safe
CURRENT[2]    : *mut Task                     // running task per core
PREV_REQUEUE[2]: *mut Task                    // task awaiting requeue after switch
PREEMPT_MODE[2]: bool                          // this core is in symmetric-preempt mode
```

Scheduling step (called from the timer IRQ on a core in `PREEMPT_MODE`, IRQs
already masked by IRQ entry):
```
fn psched(core):
    let g = RUNQ.lock()              // held ACROSS the switch
    let prev = CURRENT[core]
    let next = g.pop_front().unwrap_or(IDLE[core])
    if next == prev { drop(g); return }      // nothing else runnable
    CURRENT[core] = next
    PREV_REQUEUE[core] = prev         // DO NOT push prev yet
    next.state = RUNNING
    context_switch(prev, next)        // saves prev fully; lock still held
    finish_switch(core)               // runs in prev's context when prev later resumes
```

`finish_switch` runs as the **first thing after** any `context_switch` return
(and at the top of the fresh-task trampoline). By the time it runs, the task
that this core switched *away from* on its *previous* `psched` is fully saved:
```
fn finish_switch(core):
    let p = PREV_REQUEUE[core]
    PREV_REQUEUE[core] = null
    if p != null && p.state == READY { RUNQ_unlocked_push(p) }  // safe: p fully saved
    RUNQ.force_unlock()               // release the lock acquired by the switching core
    restore_irq_state()               // DAIF handoff (see below)
```

Key invariant: between `context_switch(prev, next)` and the matching
`finish_switch`, **the lock is held**, so no other core can pop `prev`. `prev`
is only pushed after the switch saved it AND under the lock → safe.

## Required plumbing

1. **Fresh-task trampoline** (`psched_trampoline` in `switch.S`): on first run a
   task has never executed `psched`, so it must call `finish_switch` (to requeue
   the previous task + release the lock the switching core left held) **before**
   running its entry, then unmask IRQs:
   ```
   psched_trampoline:
       mov x0, <core>        // or read MPIDR
       bl  finish_switch
       msr daifclr, #2
       blr x19               // entry
       bl  psched_task_exit  // mark DEAD, psched to next
   ```
2. **Resuming tasks** call `finish_switch` immediately after their `psched`'s
   `context_switch` returns (already in `psched` above).
3. **DAIF/IRQ-state handoff**: the lock is acquired in the IRQ handler (IRQs
   masked). It is released by `finish_switch` in a *different* stack/context.
   Since IRQs are masked throughout IRQ handling on every core, the simplest
   correct choice is: pool-preempt tasks always run with IRQs *enabled* (the
   trampoline/`finish_switch` unmask), and `psched` is only ever entered from
   the timer IRQ (already masked) → no separate DAIF save/restore needed; the
   `eret`/return path restores it. Verify ESR paths don't nest.
4. **Timer routing**: the EL1 IRQ handler must check `PREEMPT_MODE[core]` and,
   for the timer PPI, call `psched(core)` instead of the normal
   `schedule_hook`/`c1_preempt`.
5. **Entering/leaving the mode** without disturbing core 0's round-robin: the
   cleanest contained approach is a *dedicated burst*. `smpsched -p k`:
   - core 0's shell task and core 1 both set `PREEMPT_MODE[core]=true` and call
     `psched_run(core)` (an idle anchor that loops `wfi`; the timer drives it).
   - When `RUNQ` is empty and no task is `RUNNING`, both cores clear
     `PREEMPT_MODE` and return to their normal schedulers.
   - Core 0 re-entry: `psched_run(0)` is itself a normal RR task, so when it
     returns, RR continues. While active, core 0's RR is effectively paused for
     the burst (acceptable for a demo; a production version unifies RR with
     `RUNQ`).

## Verification plan

- Exactly-once **pid checksum** (already used by `smpsched`) — catches gross
  double-run / loss.
- **Migration count** — tasks observing a core change across preemptions.
- **Soak**: run the preemptive burst hundreds of times; assert checksum holds
  every time and heap is flat (reclamation). A latent two-stack race typically
  surfaces as an occasional checksum mismatch or fault under soak — so the soak
  is the real gate before merging.
- Run under QEMU `-smp 2` and also `-smp 4` if the GIC/redistributor setup is
  extended, to stress more interleavings.

## Why this was deferred

The failure mode is latent corruption, not a clean compile/boot error. It must
be gated by a sustained soak (not a single boot test) before it can be trusted —
which is a dedicated session's worth of careful implementation + validation. The
cooperative migration already shipped delivers real cross-core task movement
without this risk; this spec is the path to the involuntary variant when that
focused effort is undertaken.
