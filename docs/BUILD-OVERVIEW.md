# Fermi OS — Complete Build Overview

This document records **everything built** in the Fermi OS effort and **every
branch involved**, with what each delivered, why, and how it was verified.

> Snapshot: `integration` is **78 commits** ahead of `rust-port` and is the
> consolidated pure-Rust mainline (open as PR #16 → `main`). A separate C
> hypervisor lineage lives under `integration-hypervisor` and the `feat/hyp-*`
> / `feat/el2-*` branches.

---

## 1. The big picture: one repo, two lineages

The project began as the original **C + assembly** Fermi OS (an aarch64 / ARMv8-A
microkernel for QEMU `virt`, Cortex-A72). At commit `a2f1104`
(*"demand-paged user stack growth"*) the history **forked into two independent
lines of development**:

| Lineage | Language | Base | What it is | Consolidated branch |
|---|---|---|---|---|
| **Rust port** | pure Rust + asm | `rust-port` (off `a2f1104`) | ground-up `no_std` Rust rewrite of the whole kernel + many new features | **`integration`** |
| **C hypervisor** | C + asm | `a2f1104` directly | an EL2 type-1 hypervisor that boots Linux/FermiOS guests | **`integration-hypervisor`** |

They share early git history but are **different codebases** (Cargo vs Makefile)
and cannot be merged into one tree. This document focuses on the Rust port
(the bulk of the work) and summarizes the hypervisor lineage separately.

---

## 2. Rust port — goal & method

Convert the C kernel into **pure Rust + aarch64 assembly**, working through the
original commit history subsystem-by-subsystem, **building and boot-testing in
QEMU at every step**, then continue adding features — each on its own branch,
each individually built + boot-verified + pushed.

- Target `aarch64-unknown-none`; build `cargo build`; run `make run` (QEMU).
- Only **five** hand-written assembly files remain: `boot.S`,
  `exception/vector.S`, `sched/switch.S`, `sched/user_prog.S`, `smp.S`.
- QEMU virtio devices require `disable-legacy=on` (modern PCI IDs).

---

## 3. Rust port — conversion milestone branches

These are the incremental subsystem ports. Their content is **squashed into
`rust-port`** (the complete port); the branches remain as historical markers.

| Branch | Subsystem |
|---|---|
| `physical-memory-manager` | bitmap PMM (8 GiB) |
| `memory-management-unit` | 3-level MMU, higher-half kernel (TTBR1) + per-task TTBR0 |
| `kheap` | first-fit heap behind `#[global_allocator]` |
| `kprintf` | `core::fmt` over UART, `ksnprintf!` |
| `exception-vector-table` | trap-frame vectors, ESR/DFSC decode |
| `gicv3` | GICv3 interrupt controller bring-up |
| `timers` | ARM generic timer (drift-free tick) |
| `feat/scheduler` / `scheduler` | preemptive round-robin scheduler |
| `userspace` | EL0 user tasks + SVC syscalls |
| `pci/rng`, `pci/blk`, `pci/virtio` | PCI ECAM + VirtIO transport + drivers |
| `feat/VFS`, `fs/fat32` | VFS + FAT32 |
| `rust-port` | **the complete consolidated C→Rust port** (HEAD `017bf61`) |

`rust-port` covers: boot/UART → exception levels → PMM → MMU higher-half → heap →
trap-frame exceptions → GICv3 + timer → strings → preemptive scheduler → EL0 +
syscalls → PCI/VirtIO + 5 drivers (rng/blk/net/console/balloon) → ARP/IPv4/ICMP/
DHCP/netd → VFS + FAT32 + device nodes + fd table → /proc → ELF64 loader + fork +
exec → interactive shell. Zero C remaining; GitHub Actions CI.

---

## 4. Rust port — feature branches (all merged into `integration`)

Each branched off `rust-port` (or a stated parent), built, **boot-verified in
QEMU**, committed, pushed. Stacked chains telescope (`net-dns`→`net-tcp`/`net-ntp`;
`shell-fileops`→`fat32-rm`→`fat32-mv`→`fat32-append`; the `smp-*` chain).

### Shell / diagnostics
| Branch | What | Verified |
|---|---|---|
| `feat/shell-fileops` | `write` (FAT32 create) + `hexdump` | write→cat→hexdump |
| `feat/shell-top` | `top` dashboard | live overview |
| `feat/shell-sysinfo` | `sysinfo` summary | combined view |
| `feat/shell-rand` | `rand [n]` via VirtIO RNG | distinct bytes |
| `feat/shell-memtest` | allocator stress test | PASS, reclaims |
| `feat/shell-history` | command history + arrow recall | up-arrow re-runs |
| `feat/shell-tabcomplete` | TAB completion | `ver`+TAB→version |
| `feat/shell-blk` | raw sector I/O (`blkdump`/`blkwrite`) | sector round-trip |
| `feat/shell-wc` | `wc` line/word/byte | 2 14 73 |
| `feat/shell-stat` | `stat` vnode metadata | type/size/cluster |
| `feat/shell-uname` | `uname [-a]` | system id |
| `feat/shell-grep` | `grep <pat> <path>` | match + count |
| `feat/shell-cp` | `cp` (VFS read → FAT32 create) | round-trips |
| `feat/shell-df` | disk capacity + FAT32 usage | 16 MiB; clusters |
| `feat/proc-cpuinfo` | CPU id + `/proc/cpuinfo` + PMU | Cortex-A72 |
| `feat/exception-stats` | per-class trap counters + `traps` | svc/irq counts |
| `feat/heap-stats` | alloc/free/peak + `heapstat` | counters |
| `feat/sched-stats` | per-task CPU ticks in `ps` | idle accrues |
| `feat/sched-ctxt` | context-switch counter | grows |
| `feat/reboot-psci` | real reboot (PSCI SYSTEM_RESET) | banner twice |
| `feat/rtc` | PL031 RTC + `date` (epoch→UTC) | matches host |

### Filesystem (FAT32 write suite)
| Branch | What | Verified |
|---|---|---|
| `feat/fat32-rm` | file delete + **serialize block I/O** (SpinLock) | rm; no BLK race |
| `feat/fat32-mv` | rename (in-place 8.3) | renamed, content kept |
| `feat/fat32-append` | append (read+delete+create) + **create overwrites** + `lock_irqsave` block lock | 4→8→14 bytes, no dup |

### Networking
| Branch | What | Verified |
|---|---|---|
| `feat/net-ping-stats` | `ping [count]` + RTT/loss | 3/3, ~127 µs |
| `feat/net-arp` | ARP cache + `arp [ip]` | resolves MACs |
| `feat/net-dns` | UDP DNS resolver + `resolve` | real IP |
| `feat/net-tcp` | TCP client + `http <host>` | `HTTP/1.1 200 OK` |
| `feat/net-ntp` | SNTP client + `ntp` | network time = host |

### Process / syscalls
| Branch | What | Verified |
|---|---|---|
| `feat/elf-argv` | `argv` passing to ELF programs | echoes arg |
| `feat/sys-getrandom` | `SYS_GETRANDOM` + user hex ELF | distinct hex |
| `feat/syscall-irq-unmask` | unmask IRQs in syscall dispatch | no regression |
| `feat/blk-irq` | interrupt-driven block device | blk SPI counted |

### SMP scheduler arc (the headline progression)
| Branch | What | Verified |
|---|---|---|
| `feat/smp` | secondary core via PSCI `CPU_ON` | core1 MPIDR |
| `feat/smp-heartbeat` | secondary heartbeat | counter advances |
| `feat/smp-mmu` | core 1 in higher half (MMU + atomics + vectors) | concurrent kprintln |
| `feat/smp-print-lock` | IRQ-safe cross-core print lock | clean SMP output |
| `feat/smp-tasks` | cooperative multitasking on core 1 (2 tasks) | both advance |
| `feat/smp-preempt` | **preemptive** per-core scheduling on core 1 (own GIC+timer) | equal advance, no yield |
| `feat/smp-workqueue` | shared `SpinLock` work queue drained by **both cores** + `lock_irqsave` | 8000 jobs, exact checksum |
| `feat/smp-sched` | **symmetric run-to-completion** task scheduling (shared run queue, both cores, `context_switch`) | 120 tasks, per-core split, exact pid checksum |
| `feat/smp-migrate` | **cooperative cross-core migration** (`pool_yield` → requeue → resume on other core) | 99/100 migrated, checksum exact |
| `feat/smp-pool-reclaim` | reclaim finished tasks' stacks + `pool_join` + unique pids | heap flat over 180 tasks |
| `feat/smp-parsum` | **parallel reduction** `parsum <n>` (Σ split into 64 chunks across both cores) | 1M→500000500000, 5M→12500002500000 |

### Headline verified results
- **TCP**: `http example.com` → `HTTP/1.1 200 OK` over a real connection.
- **DNS/NTP**: resolve real IPs / fetch network time matching the host.
- **SMP**: two cores run real `context_switch`ed tasks from a shared run queue;
  tasks **migrate** between cores; **parallel sum** returns the exact answer —
  the exactly-once pid checksum proves no loss/duplication across cores.

---

## 5. Rust port — `integration` (the consolidated mainline)

`integration` = `rust-port` + all §4 feature branches merged (78 commits).
Notable consolidation work:

- Resolved the heavy `shell.rs` merge tangle by **deterministically
  reconstructing** the dispatcher + helper functions from single-source branches
  (a naive line-merge crossed function bodies).
- Resolved `net.rs` (TCP vs NTP inserted at the same anchor) by splicing.
- Caught and fixed a **latent merge defect**: a duplicated, corrupted `ping`
  arm shadowing the correct one.
- Builds clean (**zero warnings**), boots clean: MMU PASS, PING ttl=255, ELF at
  EL0, `uname`/`df`/`date`/`smpsched`/`parsum`/`ps` all work.
- **Open as PR #16 → `main`** for review/merge (`main` is never force-pushed).

Docs on `integration`: `README.md` (feature overview), `PROJECT_LOG.md` (full
session record + §13 SMP arc), `docs/preemptive-migration-design.md` (spec for
the one deferred frontier).

---

## 6. Key bare-metal-Rust engineering learnings

Real bugs found by testing (the C original never hit these):

- **Atomics fault pre-MMU** (Device memory) → lock-free `Racy<T>` before MMU,
  `SpinLock` after.
- **`core::fmt` faults pre-MMU** (unaligned copies) → aligned `uart` logging
  before MMU.
- **FP/SIMD trap (EC=0x7)** → enable `CPACR_EL1.FPEN` in `boot.S`.
- **Pre-MMU absolute relocations** (match→&str tables) fault → keep pre-MMU code
  PC-relative.
- **`context_switch` must save d8–d15** (kernel uses FP via `core::fmt`).
- **TCP checksum + frame-buffer reuse**; slirp needs an explicit handshake ACK.
- **GICv3 SPI delivery** needs IGROUPR/IPRIORITYR/IROUTER, not just PPIs.
- **Block I/O must be serialized** (single shared request header/virtqueue):
  plain `SpinLock`, upgraded to `lock_irqsave` once scheduling became preemptive.
- **The two-stack race** in SMP scheduling: a task must be fully saved before it
  can be picked by another core — handled by run-to-completion + cooperative
  yield; the involuntary-preemptive variant is specified but deferred.

---

## 7. The C hypervisor lineage (separate codebase)

Developed in **C** (Makefile build; builds here with `clang` as the
cross-compiler since `aarch64-linux-gnu-gcc` isn't installed). It is an **EL2
type-1 hypervisor** that boots guests (FermiOS and Linux) with stage-2 paging,
virtio-mmio devices, and multi-vCPU SMP guests. Consolidated tip
`integration-hypervisor` (the linear Linux-guest line) **builds and boots at
EL2** (verified: *"Fermi hypervisor online at EL2 … stage-2 enabled … dropping
to EL1 guest"*).

Branches (a different contributor's work; not merged into the Rust `integration`):

- **Linear Linux-guest track** (timestamped, each contains the earlier ones):
  `virtio-rng-mmio` → `virtio-blk-mmio` → `dedicated-guest-consoles` →
  `interactive-linux-console` → `linux-init-respawn` → `third-guest-nvm` →
  `pl011-rx-timeout-irq` → `interactive-rx-byteloss` (fix) →
  `test/virtio-blk-write-verify` → `wfi-idle-yield` → `smp-guest-2core` →
  `virtio-blk-ext4-disk` → `true-root-disk-switchroot` → `virtio-net-mmio` →
  `live-migration-precopy` → `progress/fermi-hypervisor-m1-m15`.
- **Milestone / VHE / feature branches** (divergent — not in the linear tip):
  `el2-hypervisor-fermios-guest`, `el2-type1-hypervisor-linux-guest`,
  `el2-vhe-hypervisor-fermios-guest`, and `feat/hyp-*`
  (`virtio-mmio`, `virtio-blk`, `virtio-net`, `virtio-balloon`, `vpci`,
  `vpci-msix`, `pv-console`, `fault-isolation`, `watchdog`, `weighted-sched`,
  `observability`, `live-migration`, `snapshot-restore`, `psci-cpu-off`,
  `smp-guest`).
- Docs: `docs/hypervisor-design-*`, `docs/devlog-*`.

These were **deliberately not force-merged** — VHE vs type-1 are different
architectures, and merging divergent C branches without the author's intent
would be incoherent.

---

## 8. Branch index (quick reference)

- **Mainlines**: `main` (default, untouched), `rust-port` (complete C→Rust port),
  `integration` (Rust port + all features — PR #16), `integration-hypervisor`
  (consolidated C hypervisor).
- **Rust conversion milestones** (in `rust-port`): `physical-memory-manager`,
  `memory-management-unit`, `kheap`, `kprintf`, `exception-vector-table`,
  `gicv3`, `timers`, `scheduler`/`feat/scheduler`, `userspace`, `pci/rng`,
  `pci/blk`, `pci/virtio`, `feat/VFS`, `fs/fat32`.
- **Rust feature branches** (in `integration`): see §4 (~44 branches across
  shell, FS, net, process, and the SMP scheduler arc).
- **C hypervisor branches**: see §7.
- **Docs branches**: `docs/project-log`, `docs/hypervisor-design-*`,
  `docs/devlog-*`.

---

## 9. How everything was verified

Every milestone and feature was run in QEMU (`-smp 2`) and confirmed by grepping
serial output for concrete evidence (`TTBR1 Upper Half: PASS`, `PING reply …
ttl=255`, `HTTP/1.1 200 OK`, `network time = …`, `hello from a loaded ELF64`,
`smpsched` per-core counts + exact pid checksum, `parsum … OK`, the `fermi>`
prompt). CI builds the kernel + disk, boots in QEMU, and asserts the
MMU/driver/networking/ELF/shell milestones. The hypervisor tip was build- and
EL2-boot-verified.

The discovery of *real bugs during bring-up* (FP trap, TCP checksum reuse,
GICv3 SPI routing, pre-MMU relocation hazard, FAT32 16-bit sector count,
block-I/O concurrency, the merge-tangle `ping` defect) is itself evidence the
implementation is genuinely exercised end-to-end, not stubbed.

---

## 10. The one deferred item

**Involuntary (timer-driven) preemptive cross-core migration** — a task forcibly
moved between cores by a timer interrupt. Its failure mode is *latent* two-stack
stack corruption, which a single boot test cannot certify; it requires the
lazy-requeue/`finish_switch` mechanism gated by a sustained soak test. Fully
specified in `docs/preemptive-migration-design.md`; every primitive it needs is
already built and merged. The cooperative migration that shipped delivers real
cross-core task movement without this risk.
