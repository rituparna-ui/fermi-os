# Fermi OS — C → Rust Port Plan (Lead Architect)

Scope: port the 26 subsystems specced in `docs/cref/*.md` from C+asm to pure Rust
(`no_std`) + hand-written aarch64 assembly. Target: QEMU `virt`, single core,
EL1 kernel / EL0 user. Already landed in this repo: `boot.S`, UART, MMIO, panic,
arch/cpu/sysreg scaffolding (see `src/`). This document is the authoritative
build order, the cross-cutting ABI contract, the asm-vs-Rust boundary, and the
risk register. Every section below is binding on per-subsystem implementers.

---

## 1. Topological Build Order (phased)

Ordering rules:
- A subsystem may only be ported after **all** its `depends_on` are ported, OR
  after its dependency is stubbed behind a trait/extern with a compile-time
  panic body (used to break a few genuine cycles — see notes).
- We preserve the **original C commit progression** where dependencies allow,
  and explicitly call out the two places where the original C ordering is
  **not** dependency-legal and must be reordered for the Rust port.

Legend: `[done]` already in tree · `[stub-first]` land an interface/extern stub
before its real deps · `→` "enables".

### Phase 0 — Foundation (mostly done)
| # | Subsystem | Deps satisfied by | Notes |
|---|-----------|-------------------|-------|
| 0.1 | `boot` (`boot.S`, `early_init`, `kernel_main` skeleton) | — | `[done]` keep `boot.S` asm; Rust entry points are `extern "C"`. |
| 0.2 | `strings` (mem*/str*/ksnprintf) | none | **Port first of the C code.** Zero deps; everyone needs `memset`/`ksnprintf`. `memcpy/memset/memmove/memcmp` must be exported as `#[no_mangle] extern "C"` because LLVM emits calls to them. |
| 0.3 | `uart` | strings (for printf) | `[done]` (scaffolding exists); finalize `uart_printf` on top of `kvsnprintf`. |
| 0.4 | `utils` (`print_current_el`, `dsb_sy`, ESUCCESS/EERROR) | uart | Trivial; lands the canonical `dsb_sy()` inline-asm wrapper everyone reuses. |
| 0.5 | `panic` | uart | `[done]`-ish; finalize register capture + `#[panic_handler]` bridge. |

### Phase 1 — Exception level + physical memory
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 1.1 | `exception` (vector table + dispatch) | uart; later syscall/sched/gic/timer/panic | `[stub-first]`. Land `vector.S` + `TrapFrame` + `exception_dispatch` skeleton that panics on every class. Dispatch targets (syscall/sched/gic/timer) are filled in as those land. The **trap-frame ABI (§2.1) is frozen here.** |
| 1.2 | `pmm` | strings (memset), uart, (mmu consts) | Bitmap allocator. Needs `KERNEL_VA_OFFSET`/`PHYS_TO_VIRT` const (shared with mmu, §2.4) and `PAGE_SIZE`. Port before mmu. |

### Phase 2 — Virtual memory + allocator
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 2.1 | `mmu` | pmm, uart | Builds dual identity tables, enables MMU, ASID/TCR/MAIR. Freezes the **PTE format (§2.3)** and **TTBR packing (§2.3)** shared with sched/syscall/user. |
| 2.2 | `heap` (kmalloc/kfree) | pmm, uart, strings, mmu (PHYS_TO_VIRT) | First-fit list allocator; also implement `GlobalAlloc` so `alloc::` is usable kernel-wide. |

### Phase 3 — Interrupts + time (the "EVT"/device-bring-up era)
> **Original C history note:** `pci`/`virtio`/`rng` were implemented *early*
> (before `pmm`) in the original C tree as a bring-up experiment. That ordering
> is **not dependency-legal for a clean port**: the modern VirtIO drivers need
> `mmu` (for `VIRT_TO_PHYS` on DMA rings) and the device cache/`heap`. We
> therefore **reorder**: bring up `gic`/`timer` first (they only need
> mmio+sysregs), then do `pci`/`virtio` after `mmu`+`heap` are solid. Where the
> original C relied on identity mapping pre-MMU, the Rust port relies on
> post-MMU upper-half mapping instead. This is the single biggest intentional
> deviation from the C commit order; call it out in the port commit message.

| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 3.1 | `gic` (GICv3) | uart, mmio, exception | Distributor/redistributor + sysreg IRQ ack/EOI. Wire into `exception` IRQ path now. |
| 3.2 | `timer` (generic timer, PPI 30) | gic, uart; later sched | `[stub-first]` on the `sched_wake_sleepers()` call (no-op until sched lands). |

### Phase 4 — Scheduler + syscalls + userspace
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 4.1 | `sched` | mmu, pmm, heap, timer, exception, strings; **fd (cycle)** | `[stub-first]` on `fd_table_create/destroy` (return a dummy until vfs lands). Freezes **context-switch ABI + task offsets (§2.1)**. `switch.S` stays pure asm. |
| 4.2 | `syscall` + `elf` loader | sched, mmu, pmm, heap, vfs (cycle), timer, uart, exception; net/balloon (late) | `[stub-first]` on net/balloon/vfs syscalls (return `-ENOSYS`). Freezes **syscall ABI (§2.2)**. |
| 4.3 | `user` (EL0 ABI, crt0, argv build) | exception, sched, mmu, pmm, heap, vfs, elf, timer, uart | Mostly the kernel-side `sys_exec`/argv-on-stack logic + a tiny EL0 crt0/syscall-wrapper crate. |

### Phase 5 — Filesystem + device namespace
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 5.1 | `vfs` | heap, strings, uart, sched; blk/fat32/proc (lazy, behind trait objs) | Resolves the sched↔vfs and syscall↔vfs cycles: vfs is built with `&'static dyn FileOperations` registrations, so blk/fat32/proc plug in later. Once vfs lands, replace the Phase-4 fd stubs with the real `FdTable`. |
| 5.2 | `devices` (/dev/{console,null,zero,rng,vcons,blk}) | vfs, uart, rng, blk, vcons | Thin adapters; null/zero are pure Rust. rng/blk/vcons adapters are stubbed until Phase 6 drivers exist, then enabled. |
| 5.3 | `fat32` | blk, uart, strings, utils, vfs, heap | Needs `blk` (Phase 6) — so the *mount* call is sequenced after Phase 6 in `kernel_main`, even though the module compiles in Phase 5. |
| 5.4 | `proc` (/proc/*) | timer, pmm, heap, sched, gic, net, balloon, cpu, vfs, strings, uart | 100% safe Rust. Each generator just calls a snapshot fn on its subsystem; generators for not-yet-ported subsystems return a placeholder line. |

### Phase 6 — PCI + VirtIO transport + device drivers
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 6.1 | `pci` (ECAM enum, BAR alloc) | mmio, uart, utils | Boot-time only. |
| 6.2 | `virtio` (PCI transport, virtqueues) | pci, mmio, mmu (VIRT_TO_PHYS for rings) | Shared transport for all virtio devices. Freezes virtqueue ring layout. |
| 6.3 | `rng` (virtio-rng) | pci, virtio, mmio, mmu | Enables `/dev/rng`. |
| 6.4 | `blk` (virtio-blk) | pci, virtio, mmio, mmu, utils, uart | Enables `/dev/blk`, then `fat32_mount()`. |
| 6.5 | `net` (virtio-net + DHCP/ARP/ICMP) | pci, virtio, mmio, mmu, uart, strings | Enables `sys_net_ping`, `/proc/netinfo`. |
| 6.6 | `console` (virtio-console TX) | pci, virtio, mmio, mmu, cpu, uart, strings | Enables `/dev/vcons`. |
| 6.7 | `balloon` (virtio-balloon) | pci, virtio, virtqueue, pmm, mmio | Enables `sys_balloon`, `/proc/balloon`. |

### Phase 7 — Top-level orchestration
| # | Subsystem | Deps | Notes |
|---|-----------|------|-------|
| 7.1 | `kernel` (task_a/b/crash, task_shell, netd, sys_* wrappers, full `kernel_main` ordering) | **everything** | Finalize `early_init` → `kernel_main` boot sequence; spawn task set; start timer; enter WFI idle loop. Replace all Phase-1..6 stubs with real calls. |

### Dependency cycles & how they are broken
1. **sched ↔ fd/vfs** — sched needs `fd_table_create`; vfs needs `sched_current`.
   Break with a trait/extern stub: sched links against a weak `fd_table_create`
   returning a null table until vfs (5.1) lands; vfs's `sched` dependency is only
   `sched_current`/`sched_first_task`, already present after 4.1.
2. **syscall ↔ sched/vfs/net/balloon** — syscall dispatch references handlers
   that don't exist yet. Break with a syscall table of `Option<fn>`/stub fns that
   return `-ENOSYS`; fill entries in as each provider lands.
3. **timer ↔ sched** — `timer_handle_irq` calls `sched_wake_sleepers`. Break with
   a no-op until 4.1, then wire.
4. **exception ↔ {syscall,sched,gic,timer,panic}** — exception is `[stub-first]`
   (1.1); its dispatch arms are filled incrementally. Never reorder exception
   after its consumers — it must exist (panicking) before anything can trap.
5. **pmm ↔ mmu** (consts only) — both need `PAGE_SIZE`/`KERNEL_VA_OFFSET`/
   `PHYS_TO_VIRT`. Put these in a dependency-free `mm::consts` module imported by
   both, so neither depends on the other for constants.

---

## 2. Cross-Cutting Concerns (the frozen contracts)

These are **shared invariants** touched by multiple subsystems and by both Rust
and asm. They must be defined **once**, in one module, and verified with
`const _: () = assert!(...)` / `core::mem::offset_of!` at compile time. A change
here is an ABI break across `vector.S`, `switch.S`, `exception`, `sched`,
`syscall`, and `user`.

### 2.1 Trap-frame ABI + context-switch frame (vector.S ⇄ exception ⇄ switch.S)

**Trap frame: exactly 688 bytes, 16-byte aligned.** Defined once as
`#[repr(C)] struct TrapFrame` in `exception::trap_frame`. `vector.S` allocates
`FRAME_SIZE = 688` and stores fields at these byte offsets:

| Offset | Field | Size |
|--------|-------|------|
| 0   | x0..x30 (`regs[31]`) | 248 |
| 248 | ELR_EL1 | 8 |
| 256 | SPSR_EL1 | 8 |
| 264 | ESR_EL1 | 8 |
| 272 | FAR_EL1 | 8 |
| 280 | **SP_EL0** (EL0 entry only; absent from the C `trap_frame_t` struct) | 8 |
| 288 | q0..q7, q16..q31 (caller-clobbered SIMD) | 256 |
| 672 | FPSR | 8 |
| 680 | FPCR | 8 |

Rules every implementer must honor:
- The C `trap_frame_t` struct is only the **first 40 bytes** (regs+elr+spsr+esr+far).
  `sp_el0` is at **offset 280** and is **not** a struct field — access it via raw
  pointer `*(frame as *mut u64).add(35)`. The `user`/`syscall` ports rely on this
  (e.g. `sys_exec` rewrites `frame_raw[35]`).
- Save **only caller-clobbered SIMD** (q0–q7, q16–q31). q8–q15 are callee-saved
  and are preserved by `context_switch` instead. Saving the wrong set silently
  corrupts preempted-task FP state.
- All `stp q,q,[sp,#imm]` use ×16-scaled offsets; 688 was chosen so every SIMD
  store fits the signed-7-bit immediate range. Do not change the size casually.
- SPSR_EL1.M[3:0] == 0 ⇒ returning to EL0 ⇒ restore SP_EL0. Else leave it.

**Context-switch frame: exactly 160 bytes** (`CONTEXT_SWITCH_FRAME`), 16-byte
aligned: x19–x30 (12×8=96) + d8–d15 (8×8=64). Lives below the trap frame on the
kernel stack. `fork_return` restores a 288-byte slice (GPRs+ELR+SPSR+SP_EL0).

**Task struct asm-critical offsets** (hardcoded in `switch.S`): `TASK_SP = 0`,
`TASK_TTBR0 = 40`. Enforce with:
```rust
const _: () = assert!(core::mem::offset_of!(Task, sp) == 0);
const _: () = assert!(core::mem::offset_of!(Task, ttbr0) == 40);
const _: () = assert!(core::mem::size_of::<TrapFrame>() <= 288); // C struct part
```
`switch.S`, `vector.S`, and `fork_return` must agree on these numbers; a
mismatch is silent memory corruption.

### 2.2 Syscall ABI (user ⇄ exception ⇄ syscall ⇄ kernel)

- **AAPCS64-style:** `x8 = syscall number`, `x0..x7 = args`, return in `x0`.
- Entry via `SVC #0` from EL0 → `EC_SVC_AARCH64 (0x15)` → `exception_dispatch`
  → `syscall_dispatch(&mut TrapFrame)`.
- `syscall_dispatch` writes the return value into `frame.regs[0]` **except**
  `SYS_EXEC` (rewrites the whole frame and returns early without touching x0) and
  the noreturn calls (`SYS_EXIT`).
- Numbers (frozen, must match C): `READ=0 WRITE=1 OPEN=2 CLOSE=3 EXIT=4 YIELD=5
  SLEEP=6 GETPID=7 LSEEK=8 UPTIME=9 NET_PING=10 KILL=11 FORK=12 EXEC=13 BALLOON=14`.
- **Preemption:** `syscall_dispatch` must `msr daifclr, #2` early to unmask IRQs so
  long syscalls (e.g. UART read) are preemptible. Therefore syscall handlers must
  be re-entrancy-safe w.r.t. global state.
- **User pointer validation:** every buffer arg goes through `user_buf_ok(ptr,len)`
  (range `[0, USER_STACK_TOP)`, zero-length always OK) and strings through
  `user_str_ok` (scan ≤ 4096 for NUL). Validation only checks the VA range, not
  page mapping — an unmapped in-range page still faults in the kernel (known gap,
  out of scope).
- **`fork_return`** is a special asm entry: child eret's with x0=0; parent gets
  child pid normally.

### 2.3 Page-table entry format + TTBR packing (mmu ⇄ sched ⇄ syscall ⇄ user)

Define **once** in `mm::mmu::consts` (or `mm::consts`), imported everywhere:
```rust
pub const PTE_VALID: u64 = 1 << 0;
pub const PTE_TABLE: u64 = 1 << 1;          // table or page (L3)
pub const fn pte_attridx(i: u64) -> u64 { i << 2 }
pub const PTE_AP_RW:     u64 = 0 << 6;      // EL1 RW, EL0 none
pub const PTE_AP_RW_EL0: u64 = 1 << 6;      // EL1 RW, EL0 RW
pub const PTE_AP_RO:     u64 = 2 << 6;      // EL1 RO, EL0 none
pub const PTE_AP_RO_EL0: u64 = 3 << 6;      // EL1 RO, EL0 RO
pub const PTE_SH_INNER:  u64 = 3 << 8;
pub const PTE_AF:  u64 = 1 << 10;
pub const PTE_NG:  u64 = 1 << 11;           // non-global: ASID-tagged (user)
pub const PTE_PXN: u64 = 1 << 53;
pub const PTE_UXN: u64 = 1 << 54;
pub const PTE_ADDR_MASK: u64 = 0x0000_FFFF_FFFF_F000; // [47:12]
```
Standard flag recipes (used identically by sched stack mapping, syscall ELF
mapping, and user stack growth):
- **User code (RX):** `ATTRIDX(1)|AP_RO_EL0|PXN`
- **User rodata (R):** `ATTRIDX(1)|AP_RO_EL0|PXN|UXN`
- **User data/stack (RW):** `ATTRIDX(1)|AP_RW_EL0|UXN|PXN`
- All user PTEs **must** set `PTE_NG` so they are ASID-tagged; kernel PTEs leave
  nG=0 (global). Forgetting nG ⇒ stale cross-task TLB hits.

**TTBR packing** (mmu ⇄ sched, ASID lives in TTBR0[63:48]):
```rust
pub const TTBR_ASID_SHIFT: u64 = 48;
pub const TTBR_BADDR_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;
pub const fn ttbr_pack(baddr: u64, asid: u16) -> u64 {
    (baddr & TTBR_BADDR_MASK) | ((asid as u64) << TTBR_ASID_SHIFT)
}
```
ASID space `[1,65535]`, 0 reserved for kernel/idle. On allocator wrap → `tlbi
vmalle1` then reset to 1 (see Risk R3).

### 2.4 VIRT_TO_PHYS / PHYS_TO_VIRT convention (mm ⇄ all DMA drivers)

`KERNEL_VA_OFFSET = 0xFFFF_0000_0000_0000`. These are **pure arithmetic macros,
not instructions**, and live in `mm::consts`:
```rust
pub const KERNEL_VA_OFFSET: u64 = 0xFFFF_0000_0000_0000;
pub const fn phys_to_virt(pa: u64) -> u64 { pa + KERNEL_VA_OFFSET }
pub const fn virt_to_phys(va: u64) -> u64 { va - KERNEL_VA_OFFSET }
```
Hard rules:
- `virt_to_phys` is **only** valid for upper-half kernel-mapped addresses
  (TTBR1). All virtqueue rings, bounce buffers, and DMA descriptors must live in
  kernel `.bss`/heap (kernel VA) so `virt_to_phys` is meaningful. Never hand a
  user VA to `virt_to_phys`.
- During page-table walks **after MMU enable**, dereference PTE physical
  addresses through `phys_to_virt` (routes via TTBR1) — never raw-deref a PA,
  because TTBR0 may be a sparse user table that doesn't map it.
- Pre-MMU (`early_init`) code operates on raw PAs; post-MMU code operates on VAs.
  Mixing the two is the classic boot-time fault.

### 2.5 Global mutable state — how Rust `no_std` guards each kind

No heap-free std `Mutex`; we use `core` + a tiny `spin`-style lock. Selection
guide (apply per-subsystem):

| Pattern | Use for | Mechanism |
|---------|---------|-----------|
| **Cold-init, then immutable** | cpu CpuInfo, mmu boot L0 tables, vector base | `OnceCell`/`LazyLock`-style once-init; no runtime lock. |
| **Scalar counters / flags read anywhere, written in one place** | timer `tick_count`/`freq`, pmm `used_pages`, gic IRQ counters, driver "ready" flags, fn-ptr callbacks | `AtomicU64`/`AtomicUsize`/`AtomicBool` (Relaxed on single core; document SeqCst for ring idx where device ordering matters). |
| **Structured state mutated post-boot by possibly-preempting code** | pmm bitmap alloc/free, heap free-list, virtio device singletons, balloon, vfs vnode pool & fd tables, fat32 volume | `SpinLock<T>` (IRQ-unsafe) **or** explicit DAIF masking. For anything reachable from both a syscall and the timer IRQ, the lock must mask IRQs (`SpinLockIrqSafe`) to avoid deadlock. |
| **Scheduler run queue / current / dead_list** | sched | **No lock**: mutated only with IRQs masked (`daifset #2`). Wrapped in `UnsafeCell`; safety argument is "IRQs off during mutation + single core". Document this loudly. |
| **Exception/trap path** | TrapFrame access in dispatch | `&mut TrapFrame` handed in from asm; no lock — dispatch is single-threaded per CPU. |

Single-core assumption is pervasive and **intentional** for the port. Every
`UnsafeCell`/Relaxed-atomic/no-lock decision above is justified by "single CPU +
IRQ masking", and each such site must carry a `// SAFETY (single-core):` comment
naming the invariant, so SMP work later has a checklist.

---

## 3. Assembly vs. Safe Rust boundary

### Must stay hand-written assembly
Delivered either as `.S` files included via `core::arch::global_asm!` (multi-
instruction control-flow / save-restore) or `core::arch::asm!` (single sysreg
ops). Naked-fn alternatives are discouraged — prefer explicit `.S`.

**`.S` via `global_asm!` (control flow, SP/eret, exact frame layout):**
- `boot.S` — `_start`, secondary-CPU park, physical stack setup, MMU enable
  sequence + relocation to upper half. `[done]`, keep.
- `vector.S` — vector table (2 KiB aligned), `VECTOR_ENTRY*` macros,
  `exception_common` save/restore + `eret`. Owns the 688-byte frame.
- `switch.S` — `context_switch`, `task_trampoline`, `kernel_task_trampoline`,
  `fork_return`. Owns 160-byte frame, TTBR0 swap, `eret` to EL0/EL1.

**`asm!` inline (single sysreg / barrier; no Rust equivalent):**
- All `mrs`/`msr` sysreg I/O: MIDR/CTR/ID_* (cpu), PMCR/PMCCNTR (cpu),
  CurrentEL (utils), VBAR/ELR/SPSR/ESR/FAR/SP_EL0/FPSR/FPCR (exception),
  MAIR/TCR/TTBR0/TTBR1/SCTLR (mmu), CNTFRQ/CNTPCT/CNTP_CVAL/CNTP_CTL (timer),
  ICC_SRE/IAR1/PMR/IGRPEN1/EOIR1 + `daifclr #2` (gic), DAIF mask/unmask (sched,
  syscall, panic), CPACR_EL1 (boot fp/simd enable).
- Barriers/maintenance: `dsb sy` (canonical `dsb_sy()` in utils, reused by every
  virtio driver), `dsb ish`, `isb`, `wfi` (panic/idle), `tlbi
  {vmalle1,aside1,vae1}`, cache maint `dc cvau`/`ic ivau` (syscall ELF icache
  sync), `svc #0` (user syscall wrappers), `hvc #0` (PSCI reboot in shell).
- **Volatile MMIO** is `read_volatile`/`write_volatile` (not asm) but is
  similarly load-bearing — never let the optimizer elide/reorder device access.

### Can be safe (or thin-unsafe) Rust
- `strings` (pure; `memcpy` etc. are `unsafe extern "C"` but logic is plain).
- `pmm` bitmap math, `heap` free-list, `mmu` table-building logic & PTE encode,
  exception **dispatch/decoding** (EC/DFSC/FAR classification), gic init logic &
  counters, timer tick bookkeeping, all virtqueue *logic* (ring index math,
  descriptor chaining), pci enumeration/BAR sizing, fat32 entirely, vfs path
  resolution & vtable dispatch, **proc entirely (zero asm)**, devices adapters,
  net L3 (checksums/DHCP/ARP/ICMP), shell tokenizer & command handlers.
- Rule of thumb: hardware *touch* (sysreg, barrier, MMIO, eret, TLB) is asm/
  volatile; hardware *policy* (what to map, which block to allocate, how to
  route a syscall) is safe Rust.

---

## 4. Risk Register (highest first)

| # | Risk | Why it's dangerous | Mitigation |
|---|------|--------------------|------------|
| **R1** | **Trap-frame / context-switch ABI drift** between `vector.S`, `switch.S`, `exception`, `sched`, `syscall`. | Off-by-8 offset = silent memory corruption, intermittent crashes that look like anything. `sp_el0@280` is not a struct field — easy to mis-port. | Freeze §2.1 first (Phase 1.1). `offset_of!`/`size_of!` `const _: () = assert!` guards in both exception and sched. One shared constants file consumed by the `.S` (via `global_asm!` `const` operands) and Rust. Golden test: trap, dump frame, diff offsets. |
| **R2** | **Pre-MMU vs post-MMU addressing** (PA vs VA, identity vs upper-half). | `early_init` runs at PA; `kernel_main` at VA. A stray VA deref before MMU or PA deref after = instant fault. pmm bitmap pointer relocation (`pmm_relocate_upper`) is a classic trap. | Encode boot phase in the type system (`BootCtx`/`UpperHalf` ZST markers per boot.md). Centralize `phys_to_virt`/`virt_to_phys` (§2.4); forbid raw PA deref in walk code. Keep MMU-enable sequence verbatim in `boot.S`. Test `mmu_run_tests()` while still on identity TTBR0. |
| **R3** | **ASID lifecycle**: nG tagging, wraparound flush, TLBI-before-free ordering. | Missing nG ⇒ cross-task data leak (security). Missing wrap flush or free-before-TLBI ⇒ stale TLB → use-after-free of physical pages (security + corruption). | All user PTE recipes in §2.3 include `PTE_NG`. `sched_asid_alloc` wrap path issues `tlbi vmalle1` before reset. `sched_reap` issues `tlbi aside1` **before** `mmu_free_user_tables`. Add a debug assert that any user mapping has nG set. |
| **R4** | **VirtIO ordering: DSB placement + volatile `used->idx`** across rng/blk/net/console/balloon. | Miss one `dsb_sy()` after an MMIO write, or let the optimizer cache `used->idx`, and the device silently desyncs → boot hangs in a poll loop with no error. | Single canonical `dsb_sy()`; code-review checklist "every state-changing MMIO write is followed by dsb_sy". All ring index reads via `read_volatile`. Port `virtio` transport once and have all five drivers reuse it (don't reimplement per driver). 10M-iter poll timeout with logging as the safety net. |
| **R5** | **Dependency-cycle stubs left unfilled** (sched↔fd, syscall↔providers, timer↔sched). | A `-ENOSYS`/no-op stub that never gets replaced compiles and "works" until a user hits that path, then fails far from the cause. | Every stub is a `todo_stub!()`-style marker that logs `UNIMPLEMENTED <name>` to UART and is grep-able. Phase 7 has an explicit "remove all stubs" checklist; CI/grep gate on `todo_stub!` before declaring done. |
| **R6** | **`memcpy`/`memset`/`memmove`/`memcmp` linkage.** | LLVM auto-emits calls to these in `no_std`; if not exported `#[no_mangle] extern "C"`, link fails or (worse) pulls a wrong impl. Overlap semantics (memcpy UB vs memmove) must match C exactly. | Port `strings` first (Phase 0.2), export the four mem* as `#[no_mangle]`. Replicate exact C signedness/NUL/overlap semantics; unit-test against documented edge cases (strncpy no-NUL-term, strchr on `\0`, memmove backward). |
| **R7** | **Icache coherency after ELF load** (`sys_exec`). | QEMU is self-coherent so missing `dc cvau`/`ic ivau`/`dsb`/`isb` *passes in emulation* but silently breaks on real Cortex-A72 — a latent landmine. | Implement the full DC CVAU→DSB→IC IVAU→DSB→ISB sequence per cache line from `CTR_EL0` line sizes, even though tests pass without it. Mark with a comment that it's load-bearing on HW, not QEMU. |
| **R8** | **Demand-paged user-stack growth window** (exception ⇄ sched). | Growing on the wrong DFSC, or mis-computing the `[STACK_TOP-MAX*PAGE, STACK_TOP-INIT*PAGE)` window, either kills valid programs or turns wild pointers into silent allocations (masking bugs). | Only translation faults DFSC ∈ {0x05,0x06,0x07} in-window call `sched_try_grow_stack`; everything else kills. Page-align FAR, enforce `stack_grown_count < GROWN_MAX`, `tlbi vae1` (VA+ASID) after map. Unit-test the classifier (`va_classify_user`). |
| **R9** | **`no_std` global-state aliasing** (UnsafeCell/atomics chosen wrong). | A `static mut`/`UnsafeCell` mutated from both a syscall and the timer IRQ without IRQ masking = data race → heisenbug. Picking Relaxed where device-visible ordering is needed = desync. | Follow the §2.5 table strictly; every unsafe-shared site carries a `// SAFETY (single-core):` invariant comment. IRQ-reachable structured state uses IRQ-masking locks. Default to SeqCst for any atomic the device also observes. |
| **R10** | **Heap coalescing physical-adjacency check + double-free detection.** | Two address-consecutive free blocks from fragmented PMM may not be *physically* adjacent; merging them corrupts the heap. Wild/double frees corrupt kernel memory. | Port the `end_of_current == next` physical-adjacency guard before any merge; `kfree` checks both `is_free` flag and `BLOCK_MAGIC_ALLOC`, and bounds-checks against registered regions **before** dereferencing the header. Run the 6-test heap suite. |
| **R11** | **Vector-table relocation across MMU** (`exceptions_init_upper`) + 2 KiB alignment. | If the linker places `vector_table` at a different VA, or alignment slips below 2048, `VBAR_EL1` points at garbage and the *next* exception triple-faults. ISB omission fetches stale vectors. | Linker script pins `vector_table` 2048-aligned; `exceptions_init_upper` re-reads `&vector_table` and re-writes VBAR + ISB after MMU. `const _: () = assert!(addr % 0x800 == 0)`-style check where possible. |
| **R12** | **Fork frame fidelity** (`sched_fork` + `fork_return`). | Child must be byte-identical to parent except x0=0; mis-copying the 688-byte frame or the user stack, or wrong `fork_return` restore size (288), yields a child that diverges subtly. | Copy parent's full trap frame verbatim, clobber `regs[0]=0`, lay 160-byte ctx frame with x30=`fork_return`. `fork_return` restores exactly 288 bytes. Test: fork, assert parent sees pid and child sees 0 at same PC. |

---

## 5. Execution notes for implementers
- One Cargo binary; subsystems are modules under `src/` mirroring the C tree
  (`src/mm/{pmm,mmu,heap}`, `src/sched`, `src/fs/{vfs,fat32,proc}`,
  `src/drivers/{pci,virtio,rng,blk,net,console,balloon}`, `src/exception`, etc.).
- Land shared constants (`mm::consts`, exception ABI, syscall numbers) **before**
  the subsystems that consume them; they are the contract surface.
- Each phase should end green: `cargo build` for the target + (where feasible)
  boot in QEMU to the furthest milestone that phase unlocks.
- Keep the three `.S` files (`boot`, `vector`, `switch`) as the only multi-
  instruction asm; everything else is `asm!` one-liners or safe Rust.

---

## 6. Hypervisor phase (EL2 Type-1 VMM) — M1–M13

A second porting phase, layered on top of the EL1 kernel above. The C original
grew an EL2 Type-1 hypervisor (commits after the original port target); this is
its pure-Rust port, milestone-for-milestone, in `src/hyp/`.

**Boot model.** Launched with QEMU `virt,virtualization=on`, the image is
entered at **EL2**. `boot.S` detects `CurrentEL == 2`, threads that decision
through callee-saved `x28` (survives `hyp_init`, the `eret`, and `zero_bss`),
calls `hyp_init`, repoints SP_EL2 at a dedicated EL2 trap stack, and `eret`s to
EL1 where the existing Fermi kernel runs unchanged as a **stage-2-translated
guest**. Entered at EL1 directly (no `virtualization=on`), the EL2 path is
skipped entirely — fully backwards compatible, and the host-QEMU CI path is
untouched. `early_init` records the entry EL via `hyp::set_booted_via_el2` so
the EL1 guest knows whether issuing an `HVC` is safe.

**Milestones.**
- **M1** EL2 bring-up: stage-2 identity map (1 GiB blocks), VTCR/VTTBR/VBAR_EL2,
  HCR_EL2.VM, eret to EL1.
- **M2** SMCCC-style HVC ABI (`src/hyp/hypercall.rs`) + ID_AA64 trap-and-emulate
  (HCR_EL2.TID3). TID3 is dropped from M11 (Linux reads ID regs natively); the
  emulation handler is retained.
- **M3** Stage-2 isolation: the `.hyp` region is split to 4 KiB and unmapped
  from the guest; guest accesses fault to EL2, are reported + poisoned + stepped
  over. Security boundary.
- **M4** Virtual interrupts: physical IRQs route to EL2 (HCR_EL2.IMO), re-injected
  as HW-linked vIRQs via ICH_LR<n>_EL2 (HW=1); guest's unmodified handler runs.
- **M5a/M5b** Second guest + cooperative world-switch, then preemptive EL2
  scheduling (CNTHP_EL2, PPI 26, 100 ms quantum, round-robin vCPUs).
- **M6** Per-guest vGIC state (ICH_LR/VMCR/AP1R0) + interrupt ownership routing.
- **M7** Per-guest FP/SIMD context switch (q0–q31 + FPSR/FPCR).
- **M8** `/proc/vms` introspection over the HVC ABI (HVC_VM_COUNT / HVC_VM_STAT).
- **M9** Guest lifecycle via PSCI SYSTEM_OFF (reap the vCPU).
- **M10** Large guest-RAM region: a "Linux slot" at guest IPA 0x40000000 backed
  by Fermi-invisible physical RAM at 9 GiB (needs QEMU `-m 10G`). M10 ran a
  self-contained `linux_stub.S` there as proof.
- **M11–M13** Real Linux guest: arm64 boot-protocol entry (PC = Image base,
  x0 = DTB), emulated GICv3 distributor/redistributor MMIO (the guest's GIC
  window is unmapped → traps to EL2 → `hyp_emulate_gic`), CNTV (PPI 27) virtual-
  timer routing to the Linux vCPU, and an SP_EL0 add to the world-switch
  context. A faulting Linux guest is reaped (keeping Fermi + the hypervisor
  alive) rather than halting.

**Testing — `ci/hyp-smoke-test.sh`.** Stage-2 translation needs a QEMU with
complete TCG stage-2 support; the host's **QEMU 3.1.0 faults at stage-2 level 0
on the eret** (an emulator limitation — EL2 bring-up itself works there). The
hyp smoke test therefore runs **QEMU ≥ 8 (via the `osdev:dev` Docker image, QEMU
8.2.2)** and asserts the full guest boot. The normal `ci/smoke-test.sh` (no
`virtualization=on`, host QEMU) remains the primary gate and is unaffected.

**Linux guest assets (M11–M13) are NOT in the repo.** Booting a real Linux to
userspace needs an external `guest/Image` (arm64 Linux kernel) + an initramfs +
a built `guest.dtb`, staged into the slot via QEMU's `-device loader` at the
IPAs in `src/hyp/mod.rs` (Image @ IPA 0x40200000, DTB @ 0x48000000). None of
these binaries exist in git history (only `guest.dts` does), so M11–M13's code
is ported and exercised on every path EXCEPT a successful Linux boot.

`hyp_create_linux_guest` **auto-detects** a staged Image by its arm64 header
magic (`0x644d5241` = "ARM\\x64" at Image byte +56). If present, it enters the
Image per the boot protocol (PC = Image base, x0 = DTB); if absent — the default
in this tree — it falls back to the self-contained M10 bring-up stub
(`linux_stub.S`), which runs from the slot's high RAM and writes `L` through the
guest's stage-2 UART mapping. So out of the box the slot is a working second
guest (the hyp smoke test asserts the stub output), and supplying the three
assets turns it into a full Linux-to-shell boot with no code changes.
