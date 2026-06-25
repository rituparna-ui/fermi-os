# Fermi OS — C → Rust Port: Project Journal

A complete record of what was done porting Fermi OS from C+assembly to pure
Rust + aarch64 assembly, and **why** each decision was made. Companion to
[`ARCHITECTURE.md`](ARCHITECTURE.md) (how the system is built) and
[`PORT-NOTES.md`](PORT-NOTES.md) (the original port plan + risk register).

---

## 1. The task

> "Start from the very first commit of this project and progressively check all
> the commits and start converting the project into a pure rust + aarch64
> assembly project."

The starting point was Fermi OS: a complete bare-metal `aarch64` (ARMv8-A)
kernel for QEMU's `virt` machine (Cortex-A72), written in **C + assembly** over
143 commits. It already had a UART, PMM, MMU/higher-half, kernel heap, exception
vectors, GICv3, timer, a preemptive scheduler with EL0 userspace, syscalls, an
ELF loader, VFS, FAT32, /proc, the full VirtIO family (rng/blk/net/console/
balloon), a networking stack (ARP/IPv4/ICMP/DHCP), and an interactive EL0 shell.

The goal: re-implement all of it in **pure Rust + aarch64 assembly**, with no C.

Work happens on a dedicated branch, `rust-port-claude-20260625`, pushed to
`origin` (`github.com/rituparna-ui/fermi-os`). The original C tree is preserved
in git history at HEAD `a2f1104` and used as the authoritative reference
(`git show a2f1104:<path>`).

---

## 2. Strategy & why

**Mirror the original commit progression, one subsystem at a time, boot-testing
each.** Rather than a big-bang rewrite, each subsystem was ported in the same
dependency order the C project grew in (boot → UART → memory → exceptions →
scheduler → drivers → filesystems → networking → shell), and **booted in QEMU
after every step**. This keeps every commit a known-good, verifiable increment —
if something breaks, it's in the last small change.

**Why pure Rust toolchain (the critical early decision):** the host has **no
aarch64 GCC or GNU binutils** — only `rustc` (with the `aarch64-unknown-none`
target), the bundled `rust-lld` linker, and QEMU. The first action was to
de-risk this end-to-end: a minimal `#![no_std]` kernel that boots and prints
over UART, with assembly assembled by LLVM's integrated assembler via
`global_asm!(include_str!(...))`. It worked → the whole port uses this, with
**zero external build tools**.

**Two understanding/verification workflows** (multi-agent fan-outs) were used at
high-leverage moments:
1. Up front: an exhaustive read of all 26 C subsystems producing structured
   port specs + a topological build plan + a frozen-ABI contract + a 12-item
   risk register (now `PORT-NOTES.md`).
2. After the riskiest subsystem (EL0/scheduler/syscall/ELF): an adversarial
   ABI-verification pass (context-switch frame, ASID lifecycle, syscall+exec
   frame rewrite, ELF loader) against the C original — found **zero**
   divergences, confirming the boot tests.

---

## 3. Port phases (in order, each boot-verified)

| # | Commit | What & why |
|---|--------|-----------|
| 1 | boot skeleton | `no_std` Cargo project, `aarch64-unknown-none`, linker script, QEMU runner. Proves the toolchain. |
| 2 | UART | PL011 driver + `core::fmt::Write` → `kprint!`/`kprintln!` (supersedes the C `uart_printf`). |
| 3 | exception-level | `mrs!`/`msr!` sysreg macros; read CurrentEL. |
| 4 | PMM | bitmap page allocator over 8 GiB; `SpinLock` introduced. |
| 5 | MMU + higher-half | 4-level tables, identity TTBR0 + high TTBR1, enable MMU, jump to upper-half `kmain`. Highest-risk early step. |
| 6 | heap | first-fit `kmalloc`/`kfree` + `GlobalAlloc` → `alloc::` (Box/Vec/String) available kernel-wide. |
| 7 | exception vectors | 688-byte trap frame, `vector.S`, ESR/DFSC decode, panic handler. |
| 8 | GICv3 + timer | interrupt controller + periodic tick → live preemption. |
| 9 | scheduler (v1) | round-robin kernel (EL1) tasks, context switch, reaping. |
| 10 | PCI | ECAM enumeration + BAR assignment. |
| 11 | VirtIO + RNG | PCI transport + split virtqueue + virtio-rng (DMA entropy). |
| 12 | virtio-blk | sector read/write; shared `device_init_handshake` factored out. |
| 13 | console + balloon | virtio-console TX + virtio-balloon. |
| 14 | VFS + devices | vnode tree, fd tables, `FileOperations` vtables, `/dev/*`. |
| 15 | **EL0 userspace** | scheduler EL0 extension (per-task TTBR0/ASID, `task_trampoline` eret), SVC syscall dispatch, ELF loader. The #1-risk cluster. |
| 16 | /proc + cpuinfo | synthetic fs + CPU id/PMU. |
| 17 | FAT32 (read) | BPB/FAT-chain/8.3/lazy VFS lookup. |
| 18 | networking | virtio-net + ARP/IPv4/ICMP + DHCP client. |
| 19 | shell + netd | interactive EL0 shell + EL1 background pinger. |

After phase 19 the **entire OS was ported and boot-verified** — every C
subsystem re-implemented in Rust.

---

## 4. Bugs found and fixed (and why they mattered)

Real defects surfaced by testing — the payoff of boot-testing every step:

- **Pre-MMU Device-memory alignment trap.** With the MMU off, RAM is
  Device-nGnRnE, so `core::fmt`'s unaligned accesses fault. Fix: early logging
  uses aligned UART helpers; `core::fmt` is used only after MMU enable.
- **Pre-MMU absolute-VA data fault.** Rust `match`→`&str` tables and vtables
  materialize *absolute* upper-half pointers that aren't mapped pre-MMU. Fix:
  `print_current_el` and other such code run *after* `mmu::init`.
- **DHCP IPv4 checksum bug.** `udp_build` reused a frame buffer across
  DISCOVER→REQUEST without zeroing the IP checksum field, so the REQUEST carried
  a stale checksum and slirp dropped it → no ACK. Found by **byte-diffing the
  REQUEST frame against the C original** in Docker. Fix: zero the header fields
  before recomputing. (The original C worked; this was a port omission.)
- **C-style overflow guards (security).** `a + b < a` overflow checks in the
  ELF loader and `user_buf_ok` *panic in debug builds* before the comparison,
  so the guard meant to reject malformed ELFs / hostile pointers crashed the
  kernel instead. Found by **clippy**. Fix: `checked_add`.
- **reboot from EL0.** The C shell issued `hvc #0` from EL0 — architecturally
  undefined with no EL2, it faults rather than reboots (verified: the original
  C faults too). Fix: a `SYS_REBOOT` syscall that attempts PSCI from EL1 and
  degrades gracefully ("PSCI unavailable") on the QEMU virt config.
- **FAT32 duplicate-create.** `create()` didn't check for an existing name, so
  re-running on a re-used disk produced duplicate directory entries. Found by
  the new `ls`. Fix: refuse duplicates; made boot FS tests idempotent.

---

## 5. Post-port hardening (why each)

Once feature-complete, work shifted to making the port trustworthy and
maintainable:

- **CI + smoke-test.** `ci/smoke-test.sh` boots the kernel headless in QEMU and
  asserts subsystem milestones + drives the EL0 shell over stdin;
  `.github/workflows/ci.yml` runs `clippy -D warnings` + debug/release builds +
  the smoke-test on every push. So nothing we verified can silently regress.
- **Idiomatic-Rust refactor.** Replaced the `static mut` footgun (a hard error
  in Rust 2024; invites aliasing UB) with `SyncUnsafeCell<T>` in the scheduler /
  VFS / MMU, encapsulating the "single-core, hand-managed aliasing" invariant in
  one audited place. Enabled `#![deny(unsafe_op_in_unsafe_fn)]` so every unsafe
  operation is explicitly marked with a `// SAFETY` rationale. (Driver DMA rings
  kept as `addr_of!`-accessed statics — the sound, recommended DMA pattern.)
- **Stress tests** for every reachable risk-register item:
  - CHURN — 48 task create/exit/reap, zero page leak (teardown + ASID recycle).
  - HEAP STRESS — >1 MiB alloc forces `expand`; churn → zero byte leak.
  - FD STRESS — 64-fd exhaustion, reuse, double-close, bad-fd.
  - FORK STRESS — 16 forks (688-byte frame copy + `fork_return` + reap).
  - FAT32 STRESS — 30-file create/read across a multi-sector directory.
  - ASID WRAP — seeds the counter to 65534 and crosses 65535→1, exercising the
    global-TLB-flush path (risk R3) that needs 65535 task creations to hit
    naturally.
- **Docs.** `ARCHITECTURE.md` (subsystem map, boot flow, memory layout, frozen
  ABIs, concurrency model, test matrix) + this journal.

---

## 6. New features added beyond the original

The original FS could read and create-in-root only. Added (each: kernel impl +
syscall + shell builtin + test + mtools-verified on disk):

- **`ls` / `readdir`** — directory enumeration (FAT32 on-disk + in-memory
  `/dev`, `/proc`), via `SYS_READDIR`.
- **`mkdir`** — directory creation with proper `.`/`..` entries, nested paths,
  via `SYS_MKDIR`.
- **`rm`** — file / empty-directory removal (free the cluster chain + mark the
  dir entry `0xE5`), via `SYS_RM`.
- **subdirectory `create`** — `create("SUB/FILE")` resolves the parent dir
  (the C `create` was root-only; the port had dropped parent-walking).

---

## 7. Key technical facts (frozen contracts)

- **Memory:** kernel linked at VA `0xFFFF_0000_4000_0000`, loaded at PA
  `0x4000_0000`; `KERNEL_VA_OFFSET = 0xFFFF_0000_0000_0000`. 8 GiB RAM, 4 KiB
  pages, 48-bit VA. User space: text @ `0x40_0000`, stack top @ `0x80_0000`,
  ASID-tagged (nG=1), demand-paged to 256 KiB.
- **Trap frame:** 688 bytes; SP_EL0 at byte offset 280 (not a struct field).
- **Context-switch frame:** 160 bytes (x19–x30 + d8–d15); `Task.sp` @ 0,
  `Task.ttbr0` @ 40 — asserted via `offset_of!`.
- **Syscalls (x8 = number):** READ 0, WRITE 1, OPEN 2, CLOSE 3, EXIT 4, YIELD 5,
  SLEEP 6, GETPID 7, LSEEK 8, UPTIME 9, NET_PING 10, KILL 11, FORK 12, EXEC 13,
  BALLOON 14, REBOOT 15, READDIR 16, MKDIR 17, RM 18. (0–14 match the original
  C; 15–18 are additions.)
- **Only 3 hand-written `.S` files:** `boot.S`, `vector.S`, `switch.S` (via
  `global_asm!`). Everything else is Rust + inline `asm!` for single sysreg ops.
- **Concurrency:** single core. `SpinLock<T>` for post-boot structured state;
  `SyncUnsafeCell<T>` for IRQ-masked scheduler/VFS state; `addr_of!` statics for
  DMA rings. The run queue is intentionally lock-free (IRQ-masked) so the
  timer-IRQ schedule path can't deadlock.

---

## 8. Final state

- **Branch:** `rust-port-claude-20260625` (pushed), 41 commits on top of the
  original C HEAD `a2f1104`.
- **~10,000 lines** of pure Rust + aarch64 assembly across
  `src/{arch,klib,mm,exception,sched,syscall,drivers,fs,user}`.
- **Clippy-clean** (`-D warnings`) in debug and release; zero TODO/unimplemented
  markers; release build boots cleanly (no UB exposed by optimization).
- **Verified by:** per-boot self-tests (MMU/heap/exception/rng/blk), six stress
  tests, and the CI smoke-test's 29 assertions covering boot milestones +
  interactive shell (`pid`/`mkdir`/`rm`/`ls`/`fork`/`balloon`/`hexdump`/`exec`).
- **No PR opened** (per instruction) — work continues on the branch.

The OS boots into the higher half, runs an interactive EL0 shell over the UART,
acquires a DHCP lease and pings the gateway, reads/writes/lists/removes FAT32
files, loads and execs ELF binaries from disk, and survives deliberate task
faults (killing only the offending task) — entirely in pure Rust + assembly.
