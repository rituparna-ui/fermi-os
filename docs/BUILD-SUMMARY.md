# Fermi OS — Build Summary & Branch Map

This document records **what was built on this line of work**, **how it is
organized**, and **how it relates to the other branches** in the `fermi-os`
repository. It is written from the perspective of the work delivered on the
branch [`rust-port-claude-20260625`](#the-deliverable-branch), and is deliberate
about attribution: the repo is shared by several parallel efforts, and this
document only claims the one branch below.

Companion docs (all on this branch): [`ARCHITECTURE.md`](ARCHITECTURE.md) (how
the system is built), [`PROJECT-JOURNAL.md`](PROJECT-JOURNAL.md) (the narrative
record + rationale), [`PORT-NOTES.md`](PORT-NOTES.md) (the original port plan +
risk register + the hypervisor phase notes).

---

## 1. What this is, in one paragraph

Fermi OS is a bare-metal `aarch64` (ARMv8-A) operating system for QEMU's `virt`
machine (Cortex-A72). It began as a C + assembly kernel. The work recorded here
is a **complete, from-scratch re-implementation in pure Rust + aarch64
assembly** — no C, no GCC, no GNU binutils — followed by **a second phase that
ports the project's EL2 Type-1 hypervisor**, so the same image can either boot
as a plain EL1 kernel or, with `virtualization=on`, boot at EL2 and run itself
(plus a second guest) as virtual machines.

Two things were built, in order:

1. **The kernel** — every subsystem of the C original, re-implemented in Rust,
   then hardened by three adversarial bug-hunts.
2. **The hypervisor** — the C project's later EL2 work (milestones M1–M13),
   ported milestone-by-milestone, then adversarially audited.

Everything is on one branch, boot-verified under QEMU, and CI-gated.

---

## 2. The deliverable branch

| | |
|---|---|
| **Branch** | `rust-port-claude-20260625` |
| **Worktree** | `/local/home/rituu/fermi-claude-rs` |
| **Remote** | `origin/rust-port-claude-20260625` (pushed, in sync) |
| **Base** | C commit `a2f1104` (`feat(mm): demand-paged user stack growth…`) |
| **Commits on top of base** | **64** |
| **Size** | ~12,500 lines of Rust + aarch64 assembly |
| **Status** | clippy `-D warnings` clean, `cargo fmt` clean, debug + release build clean, both smoke tests green |
| **PRs** | none opened (by instruction — work stays on the branch) |

### Why `a2f1104` is the base

`a2f1104` was HEAD of the C project's main line when the port started, and it is
an **ancestor of `main`**. The C project later grew 17 more commits (the
hypervisor, M1–M13) *after* that point — so the kernel port targeted `a2f1104`
exactly, and the hypervisor phase deliberately went back and ported those 17
newer commits too. The original C tree is preserved in git history and was used
as the authoritative reference throughout (`git show a2f1104:<path>`,
`git show main:<path>`).

> Note: the branch `fermi-claude-rs` (commit `0f15b9a`) is **not** a separate
> effort — it is an early point on *this same* port line (it appears as commit
> #38 in the history below). It exists because of a one-time branch mix-up early
> on (FAT32 was committed there before the work was consolidated onto
> `rust-port-claude-20260625`); it carries no unique work.

---

## 3. The kernel port (commits 1–37 of 64)

Ported feature-by-feature, mirroring the C commit progression. **Each phase
ended green**: it built for `aarch64-unknown-none` and booted in QEMU to the
furthest milestone that phase unlocked.

| Phase | Subsystem | What landed |
|------:|-----------|-------------|
| 1 | boot skeleton | `no_std` Cargo project, linker script, QEMU runner — proves the pure-Rust toolchain |
| 2 | UART | PL011 driver + `core::fmt::Write` → `kprint!`/`kprintln!` |
| 3 | exception level | `mrs!`/`msr!` sysreg macros |
| 4 | PMM | bitmap page allocator over 8 GiB |
| 5 | MMU + higher-half | 4-level tables, identity TTBR0 + high TTBR1, MMU enable, jump to upper half |
| 6 | heap | first-fit `kmalloc`/`kfree` + `GlobalAlloc` → `alloc::` available kernel-wide |
| 7 | exception vectors | 688-byte trap frame, `vector.S`, ESR/DFSC decode, panic handler |
| 8 | GICv3 + timer | interrupt controller + periodic tick → live preemption |
| 9 | scheduler | round-robin EL1 tasks, context switch, reap |
| 10 | PCI | ECAM enumeration + BAR assignment |
| 11 | VirtIO + RNG | PCI transport + split virtqueue + virtio-rng |
| 12 | virtio-blk | sector read/write; shared device-init handshake |
| 13 | console + balloon | virtio-console TX + virtio-balloon |
| 14 | VFS + devices | vnode tree, fd tables, `FileOperations` vtables, `/dev/*` |
| 15 | **EL0 userspace** | per-task TTBR0/ASID, `eret` to EL0, SVC syscalls, ELF loader |
| 16 | /proc + cpuinfo | synthetic fs + CPU id/PMU |
| 17 | FAT32 (read) | BPB/FAT-chain/8.3/lazy VFS lookup |
| 18 | networking | virtio-net + ARP/IPv4/ICMP + DHCP client |
| 19 | shell + netd | interactive EL0 shell + EL1 background pinger |

After phase 19 the **entire C kernel was re-implemented in Rust and
boot-verified.** The remaining kernel commits added value beyond the original:

- **FAT32 write path** + create/round-trip test.
- **Filesystem features the C lacked**: `ls`/`readdir`, `mkdir` (nested),
  `rm` (file + empty dir), subdirectory `create`.
- **Idiomatic-Rust hardening**: replaced `static mut` with `SyncUnsafeCell`,
  enabled `#![deny(unsafe_op_in_unsafe_fn)]` and wrapped every unsafe site with
  a `// SAFETY (single-core)` invariant.
- **A stress-test suite**: task-churn (leak/UAF), heap exhaustion+expand, fd
  exhaustion/reuse, fork, multi-file FAT32, FAT32 delete/reuse churn, and an
  ASID-wraparound test (drives the 16-bit ASID counter across 65535→1).
- **CI**: a headless QEMU boot smoke-test + GitHub Actions (`fmt --check`,
  `clippy -D warnings`, debug+release build, smoke-test ×2).

### Three adversarial bug-hunts (commits ~21, 18, 17)

The ported kernel was audited in three passes (per-dimension finders →
adversarial verifiers against the C original + the frozen ABI → ranked
synthesis). **15 real, behavior-changing bugs were found and fixed** — almost
all *latent* (only triggered by precise interrupt timing or weakly-ordered
hardware that QEMU doesn't emulate), so none were findable by functional
testing alone:

- Concurrency: single-core deadlocks/races (IRQ-reachable locks → introduced
  `SpinLockIrqSafe`; the timer/`IRQ_COUNTS`/UART-interleave cases).
- DMA ordering: missing `dsb_sy()` after state-changing VirtIO MMIO writes;
  non-volatile reads of device-written ring indices; `DRIVER_OK` verification.
- Resource lifetimes: leaks / double-frees in address-space teardown.
- ABI: fork-frame fidelity.
- Boot/MMU: a missing GIC config-write barrier, an under-aligned `.bss` zero
  loop, and a stage-2 walk that could misread a block as a table.

(One audit "finding" — that `PMCR_EL0.LC` was the wrong bit — was correctly
**dismissed**: bit 6 is right for Cortex-A72; applying it would have been a
regression.)

---

## 4. The hypervisor port (commits 38–64 of 64)

A second phase ported the C project's EL2 Type-1 hypervisor — the 17 commits
that postdate the port target — milestone-for-milestone into `src/hyp/`. With
QEMU `virtualization=on` the image enters at **EL2**, sets up stage-2
translation, and `eret`s to EL1 where the rest of the kernel runs **unchanged as
a stage-2-translated guest** (vCPU 0), alongside a second guest. Without
`virtualization=on` the EL2 path is skipped entirely — fully backwards
compatible, and the EL1 CI is untouched.

| M | What |
|--:|------|
| M1 | EL2 bring-up: stage-2 identity map, `eret` to the EL1 guest; entry-EL threaded through callee-saved `x28` |
| M2 | SMCCC `HVC` hypercall ABI + `ID_AA64*` trap-and-emulate (`HCR_EL2.TID3`) |
| M3 | **Stage-2 isolation** — hyp RAM split to 4 KiB and unmapped from the guest; a guest access faults to EL2, is poisoned + stepped over (security boundary) |
| M4 | **Virtual interrupts** — physical IRQ → EL2 (`HCR_EL2.IMO`) → HW-linked vIRQ via `ICH_LR<n>_EL2`; the guest's unmodified handler runs |
| M5a/b | Second guest + cooperative world-switch, then **preemptive** EL2 scheduling (`CNTHP_EL2`, 100 ms, round-robin) |
| M6 | Per-guest vGIC state (`ICH_LR`/`VMCR`/`AP1R0`) + interrupt ownership routing |
| M7 | Per-guest **FP/SIMD** context switch (q0–q31 + FPSR/FPCR), sentinel-verified |
| M8 | `/proc/vms` live introspection over the hypercall ABI |
| M9 | Guest lifecycle via PSCI `SYSTEM_OFF` (reap the vCPU) |
| M10 | Large guest-RAM "Linux slot" at IPA `0x40000000` backed by host-invisible RAM at 9 GiB |
| M11–M13 | Real-Linux support: arm64 boot protocol, emulated GICv3 MMIO, CNTV vtimer routing, SP_EL0 ctx-switch |

**Boot-tested:** M1–M10 fully (under QEMU 8.2.2 — the host's QEMU 3.1.0 can't do
TCG stage-2). **M11–M13** are ported as code but a real Linux-to-userspace boot
can't be exercised here: no Linux `Image`/initramfs exists in the C project's
git history (only `guest.dts`). The slot **auto-detects** a staged Image by its
arm64 header magic; absent one, it runs a self-contained bring-up stub so the
second guest works out of the box (and a real Image boots with no code change
once staged — see [`PORT-NOTES.md`](PORT-NOTES.md) §6).

### Hypervisor audit

A fourth adversarial bug-hunt — 7 dimensions, per-finding triple-lens verifiers
(C-fidelity / ARM-spec / skeptic) + an independent synthesis pass — found
**zero behavior-changing bugs**. The port is a faithful translation; the
hypervisor was built carefully milestone-by-milestone against the C reference,
so unlike the original kernel it carried no latent defects.

---

## 5. Module layout (this branch)

```
src/
├── main.rs              early_init (pre-MMU) + kmain (upper-half) orchestration
├── panic.rs             kernel_panic + #[panic_handler]
├── arch/                boot.S, mrs!/msr!, CPU id + PMU, PSCI            (438 L)
├── klib/                UART, MMIO, SpinLock/SyncUnsafeCell, FmtBuf      (474 L)
├── mm/                  consts, PMM, MMU (4-level), heap                (1456 L)
├── exception/           TrapFrame + dispatch, vector.S, GICv3, timer    (860 L)
├── sched/               scheduler, switch.S, ELF loader                (1315 L)
├── syscall/             SVC dispatch, sys_exec                          (493 L)
├── drivers/             PCI ECAM + VirtIO (rng/blk/net/console/balloon) (2658 L)
├── fs/                  VFS, FAT32, /proc, /dev                         (1692 L)
├── user/                in-image EL0 programs: the shell, demo tasks    (623 L)
└── hyp/                 EL2 hypervisor: mod.rs, hypercall.rs,          (1716 L)
                         vector_el2.S, linux_stub.S
user/hello.rs            freestanding EL0 ELF for the exec test          (46 L)
```

**Five hand-written `.S` files** (all via `global_asm!`): `arch/boot.S`,
`exception/vector.S`, `sched/switch.S`, `hyp/vector_el2.S`, `hyp/linux_stub.S`.
Everything else is Rust + inline `asm!` one-liners for single sysreg ops.

### Frozen ABI contracts (guarded by `offset_of!`/`size_of!` asserts)

- **Trap frame** — 688 bytes; `SP_EL0` at byte offset 280 (not a struct field).
- **Context-switch frame** — 160 bytes (x19–x30 + d8–d15).
- **EL2 trap frame** — 256-byte reservation, x0..x30; per-guest EL1 sysregs +
  vGIC + FP live in the `Vcpu` block, not the frame.
- **Syscalls** — `SVC #0`, `x8`=number, `x0..x7`=args; numbers 0–18.
- **Hypercalls** — `HVC`, fn in `x0`, args `x1`–`x3`, result `x0`; fns 0–7.

### Concurrency model (single core)

`SpinLock<T>` for post-boot structured state; `SpinLockIrqSafe<T>` for state
shared with an IRQ handler (masks IRQs while held); `SyncUnsafeCell<T>` for
IRQ-masked scheduler/VFS state and all EL2 hyp state; `addr_of!` statics for
DMA rings. The scheduler run queue is intentionally lock-free (IRQ-masked) so
the timer-IRQ `schedule()` path can't deadlock.

---

## 6. How to build, run, and test

```bash
# Build (pure Rust; no GCC/binutils)
cargo build                         # debug
cargo build --release

# Run as a plain EL1 kernel
cargo run                           # uses run.sh as the Cargo runner

# Run as a hypervisor (EL2) — needs QEMU >= 8 and -m 10G for the Linux slot
qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on \
    -cpu cortex-a72 -m 10G -nographic \
    -kernel target/aarch64-unknown-none/debug/kernel

# Tests
./ci/smoke-test.sh                  # EL1 boot + interactive shell (host QEMU)
./ci/hyp-smoke-test.sh              # EL2 hypervisor boot (QEMU >= 8, else Docker, else SKIP)
```

CI (`.github/workflows/ci.yml`) runs fmt + clippy + debug/release builds + the
EL1 smoke-test ×2 + the EL2 hyp smoke-test on every push.

---

## 7. The broader branch landscape (NOT this work)

The `fermi-os` repository is **shared by several parallel efforts** — multiple
agents/tools and human work — checked out across **7 git worktrees**:

| Worktree | Branch (at last check) | Relationship to this work |
|----------|------------------------|---------------------------|
| `/local/home/rituu/fermi-claude-rs` | **`rust-port-claude-20260625`** | **This work** (the subject of this doc) |
| `/local/home/rituu/fermi-os` | `feat/virtio-console-…` | Separate hypervisor effort |
| `/local/home/rituu/fermi-hyp` | `feat/hyp-vpci-msix` | Separate hypervisor effort |
| `/local/home/rituu/fermi-hyp2` | `fermi-hyp2` (VHE variant) | Separate hypervisor effort |
| `/local/home/rituu/fermi-kiro-rs` | `integration` | Separate Rust-port effort (`kiro`) |
| `/local/home/rituu/fermi-kiro2` | `fermi-kiro2` | Separate effort (`kiro`) |
| `/local/home/rituu/fermi-verify` | `fermi-verify` | Separate verification effort |

The remote has **~90 branches**. The large families below are **not part of
this branch** and are listed only to map the territory and avoid confusion:

- **C-kernel feature branches** — dozens of `feat/*` on top of the C kernel:
  networking (`net-tcp`, `net-dns`, `net-ntp`, `net-arp`), the shell builtins
  (`shell-grep`, `shell-cp`, `shell-top`, `shell-history`, `shell-tabcomplete`,
  …), `rtc`, `reboot-psci`, `sched-stats`, `heap-stats`, `fat32-mv`, etc.
- **SMP branches** — a multi-core line: `feat/smp`, `smp-mmu`, `smp-sched`,
  `smp-preempt`, `smp-workqueue`, `smp-parsum`, `smp-migrate`, …
- **Hypervisor branches (other efforts)** — two distinct lines beyond this one:
  `feat/el2-type1-hypervisor-linux-guest` / `progress/fermi-hypervisor-m1-m15-…`
  and the many `feat/hyp-*` branches (`hyp-vpci`, `hyp-virtio-{mmio,blk,net,
  balloon}`, `hyp-live-migration`, `hyp-snapshot-restore`, `hyp-smp-guest`,
  `hyp-watchdog`, `hyp-weighted-sched`, …), plus the date-stamped
  `…-20260625…`/`…-20260626…` branches and the `fermi-hyp` / `fermi-hyp2`
  (VHE) lines. These pursue features beyond M13 (real-Linux root disk,
  interactive console, N>2 guests, vPCI/MSI-X, live migration) and are
  independent of `rust-port-claude-20260625`.
- **Original C history** — `origin/main` and the early topic branches
  (`physical-memory-manager`, `gicv3`, `kheap`, `userspace`, `fs/fat32`,
  `pci/*`, …) are the C project this work was ported *from*.

**Bottom line:** this document and the four companion docs describe exactly one
branch — `rust-port-claude-20260625` — a self-contained, pure-Rust port of the
Fermi kernel **and** its EL2 hypervisor, audited and CI-gated. The other
branches are the surrounding ecosystem and are mentioned only for orientation.

---

## 8. Current state

The OS boots into the higher half, runs an interactive EL0 shell over the UART,
acquires a DHCP lease and pings the gateway, reads/writes/lists/removes FAT32
files, loads and execs ELF binaries from disk, and survives deliberate task
faults (killing only the offending task) — entirely in pure Rust + assembly.
Launched with `virtualization=on`, the same image is instead a Type-1
hypervisor: it boots at EL2, runs Fermi as a stage-2-isolated EL1 guest with
virtualized interrupts and preemptive EL2 scheduling, alongside a second guest.

- **64 commits**, all pushed to `origin/rust-port-claude-20260625`.
- **4 adversarial audits** (3 kernel, 1 hypervisor); 15 kernel bugs fixed, 0 in
  the hyp.
- **2 CI-gated smoke tests** (EL1 + EL2), fmt + clippy clean in debug + release.
- **No PR opened** (by instruction) — work continues on the branch.
