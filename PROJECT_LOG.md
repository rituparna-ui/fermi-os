# Fermi OS — C → Rust Port: Project Log

This document records **everything done in this effort and why**: the goal, the
porting strategy, every subsystem milestone, the bare-metal-Rust pitfalls we
hit and fixed, the full set of feature branches, and how each piece was
verified.

---

## 1. Goal

Convert the original **Fermi OS** — a bare-metal `aarch64` (ARMv8-A) kernel
written in C + assembly, targeting QEMU's `virt` machine (Cortex-A72) — into a
**pure Rust + aarch64 assembly** kernel, working through the original git
history *commit by commit / subsystem by subsystem*, building and **boot-testing
in QEMU at every milestone**.

The original was ~150 KB of C across 143 commits. The result is **pure Rust
(~7k lines) + 4 hand-written assembly files**, with zero C remaining, that boots
to an interactive `fermi>` shell.

---

## 2. Why this approach

- **Follow the commit history as a curriculum.** The original built itself up
  in a natural dependency order (boot → UART → PMM → MMU → heap → exceptions →
  GIC/timer → scheduler → syscalls → userspace → PCI/VirtIO → FS → net → shell).
  Porting in that same order meant each layer had its dependencies already in
  place and could be verified immediately.
- **Build + boot-test every milestone.** Rather than port everything then
  debug, each subsystem was compiled and run in QEMU, grepping the serial
  output for proof it worked. This caught the bare-metal-Rust pitfalls (below)
  early, at the layer that introduced them.
- **Keep assembly only where it must be.** Boot, exception vectors, context
  switch, and the embedded user program are assembly; everything else is Rust.
- **Work on a branch.** All work is on `rust-port` (and feature branches off
  it); the original C history stays intact on the other branch and is fully
  recoverable.

---

## 3. Toolchain / build / run

- `rustc` 1.85.0 stable, target **`aarch64-unknown-none`** (tier-2, precompiled
  core/alloc — no `build-std` needed).
- `qemu-system-aarch64`, `clang` + `ld.lld` (to build user ELF binaries),
  `mkfs.fat` + `mtools` (to build the FAT32 disk image).
- Build: `cargo build` → `target/aarch64-unknown-none/debug/kernel`.
- Disk: `make disk` → `build/disk.img` (FAT32 + seed files + user ELFs).
- Run: `make run` (or `cargo run`) launches QEMU with virtio rng/blk/net/
  console/balloon attached and drops you at the `fermi>` shell.
- The QEMU runner lives in `.cargo/config.toml`; the `Makefile` is a thin
  wrapper around cargo + disk creation.

**Critical QEMU detail:** virtio devices must be attached with
`disable-legacy=on` so they present *modern* PCI device IDs (blk `0x1042`,
net `0x1041`, etc.). Without it they are transitional (`0x1001`/`0x1000`) and
the modern-only drivers won't bind.

---

## 4. Module layout (pure Rust)

```
src/
  main.rs              crate root: no_std/no_main, global_asm!(boot.S),
                       early_init() + kernel_main(), panic handler, demo tasks
  boot.S               reset entry: FP enable, physical stack/BSS, MMU jump
  cpu.rs               mrs!/msr! macros, current_el, CPU id + PMU (/proc/cpuinfo)
  mmio.rs              VA-offset-routed MMIO (0 pre-MMU, KERNEL_VA_OFFSET after)
  uart.rs              PL011 driver + Device-safe pre-MMU log helpers
  print.rs             core::fmt over UART: kprint!/kprintln! (+ cross-core lock)
  sync.rs              SpinLock<T> (post-MMU) and Racy<T> (pre-MMU, lock-free)
  panic.rs             kernel_panic: sysreg dump + halt
  strings.rs           BufWriter (ksnprintf!) + cstr helpers
  mm/
    pmm.rs             bitmap physical page allocator (8 GiB)
    mmu.rs             3-level page tables, identity map, TTBR0/TTBR1, ASIDs,
                       higher-half, user-table walker/mapper, secondary enable
    heap.rs            first-fit kernel heap + #[global_allocator] + stats
  exception.rs         trap-frame vector table, ESR/DFSC decode, dispatch,
                       per-class trap stats; submodules:
    exception/vector.S   full GPR + ELR/SPSR/ESR/FAR trap frame
    exception/gic.rs     GICv3 bringup, SPI routing, per-INTID counters
    exception/timer.rs   ARM generic timer (drift-free CNTP_CVAL tick)
  sched/
    mod.rs             preemptive round-robin scheduler, Task, fork/exec,
                       sleep, kill, stats, make_kernel_task
    switch.S           context_switch (GPR+FP), EL0/EL1 trampolines, fork_return
    user_prog.S        embedded EL0 program (fork → child exec)
  syscall.rs           SVC dispatch (read/write/open/close/lseek/exit/yield/
                       sleep/getpid/uptime/net_ping/kill/fork/exec/getrandom)
  pci.rs               PCI ECAM enumeration, BAR assignment, INTx→GIC INTID
  virtio/
    mod.rs             VirtIO PCI transport: caps, handshake, status
    virtqueue.rs       split virtqueue: desc/avail/used, submit/poll
    rng.rs blk.rs net.rs console.rs balloon.rs   the five drivers
  net.rs               ARP, IPv4, ICMP, DHCP, DNS, UDP, TCP, netd
  fs/
    vfs.rs             vnode tree, path resolution, fd table, device dispatch
    devices.rs         /dev/{console,null,zero,rng,vcons,blk}
    fat32.rs           FAT32 read + create, VFS lazy lookup, dir listing
    proc.rs            /proc synthetic fs (uptime/meminfo/tasks/interrupts/...)
  shell.rs             interactive shell + builtins
  smp.rs               secondary-core bringup + core-1 scheduler
  smp.S                secondary entry trampoline
```

---

## 5. Milestones (what & why)

Each was committed separately on `rust-port` and boot-verified.

1. **Scaffold + UART** — `no_std` cargo kernel, `boot.S` via `global_asm!`,
   linker script, PL011 UART + MMIO, panic handler. *Proves the toolchain and
   boots in QEMU.*
2. **Exception level** — `mrs!`/`msr!` macros, read/print `CurrentEL` (EL1).
3. **PMM** — bitmap page allocator over 8 GiB; first-fit + contiguous.
4. **MMU + higher-half** — 3-level page tables (4 KiB granule, 48-bit VA),
   identity map for TTBR0, `+KERNEL_VA_OFFSET` for TTBR1, enable MMU+caches,
   then relink at `0xFFFF000040000000` and **jump the kernel to the upper half**
   so TTBR0 is free for per-task user mappings. *This is what makes EL0
   userspace with per-task address spaces possible.*
5. **Heap** — first-fit allocator (split/coalesce/grow), wired as the Rust
   `#[global_allocator]` so `alloc::{Vec,Box,String}` work kernel-wide.
6. **Exceptions** — full trap-frame vector table, ESR/DFSC decode, kernel panic.
7. **GICv3 + timer** — interrupt controller bringup + drift-free 10 ms tick.
8. **strings/printf** — `core::fmt` already replaces `uart_printf`; added a
   `ksnprintf!` fixed-buffer formatter + cstr helpers.
9. **Scheduler** — preemptive round-robin, per-task kernel stacks, `sleep`,
   `context_switch` saving GPRs **and FP (d8–d15)**.
10. **Syscalls + EL0** — `context_switch` swaps TTBR0; EL0 drop trampoline
    (`eret`); SVC dispatch; an embedded EL0 program.
11. **PCI + VirtIO + virtqueue** — ECAM enumeration, modern transport, split
    virtqueue, verified by the RNG driver returning DMA'd random bytes.
12. **VirtIO drivers** — RNG, block, net, console, balloon (all five).
13. **VFS + FAT32 + devices + fd table** — vnode tree, path resolution,
    per-process fd table (fd 0/1/2 → /dev/console), FAT32 read+create.
14. **Networking** — ARP, IPv4, ICMP (verified ping ttl=255), DHCP, `netd`.
15. **/proc** — synthetic fs regenerated from live kernel state.
16. **ELF64 loader + fork + exec** — loads ET_EXEC from FAT32, runs at EL0;
    `fork()` deep-copies the address space (child resumes with `x0=0`),
    `exec()` replaces the image.
17. **Shell** — interactive line editor + builtins.
18. **Cleanup** — removed all C, rewrote README + Makefile, CI smoke test.

---

## 6. Bare-metal-Rust pitfalls we hit (and fixed)

These are the things the C original never faced — diagnosed via QEMU output and
an early diagnostic exception handler. **Each was a real bug found by testing.**

- **Atomics fault before the MMU is on.** All RAM is Device-typed pre-MMU, and
  exclusive load/store (LDXR/STXR — backing `compare_exchange`/`SpinLock`)
  is illegal there → fault. Fix: a lock-free `Racy<T>` cell for pre-MMU state;
  `SpinLock` only after the MMU is on.
- **`core::fmt` faults before the MMU is on.** It copies digit pairs with a
  2-byte unaligned `copy_nonoverlapping`, and unaligned accesses fault on
  Device memory. Fix: pre-MMU code logs via simple aligned `uart` helpers;
  `kprintln!` only after the MMU is on.
- **FP/SIMD trap (ESR EC=0x7).** LLVM uses SIMD registers in `core::fmt` and
  `memcpy`, but `CPACR_EL1.FPEN` defaults to *trap*. Fix: enable FP in `boot.S`
  before any Rust runs. (This was the cause of mysterious early "hangs".)
- **Pre-MMU absolute-address relocations.** A `match`-returning-`&str` builds a
  table of *absolute* (high-VA) pointers via relocations, which fault before the
  MMU is on. Inline `&str` literals use PC-relative `adrp/add` (→ physical) and
  are fine. Fix: keep pre-MMU code free of such constructs (moved
  `print_current_el` to the post-jump path).
- **`context_switch` must save FP callee-saved regs (d8–d15)**, since the
  kernel uses FP/SIMD (via `core::fmt`); otherwise tasks corrupt each other.
- **TCP checksum + buffer reuse.** `build_tcp` reused the frame buffer without
  zeroing the checksum field — the SYN only worked because the buffer started
  zeroed; later segments checksummed over stale bytes. Plus slirp needs an
  explicit ACK to finalize the handshake before relaying data.
- **GICv3 SPI delivery.** Device (SPI) interrupts need `GICD_IGROUPR` (Group1),
  `GICD_IPRIORITYR`, and `GICD_IROUTER` configured — the initial GIC bringup
  only set up PPIs, so *no* device IRQ could fire until this was fixed.

---

## 7. Feature branches (off `rust-port`)

Each is an independent branch, individually built, **boot-verified in QEMU**,
committed, and pushed to `github.com/rituparna-ui/fermi-os`. (`net-dns`/`net-tcp`
and the `smp-*` chain are stacked where one depends on another.)

| Branch | What & why | Verified by |
|---|---|---|
| `rust-port` | Complete C→Rust port + IRQ-driven net + CI (the base) | full boot-to-shell |
| `feat/shell-fileops` | `write` (FAT32 create) + `hexdump` — basic file tooling | write→cat→hexdump |
| `feat/proc-cpuinfo` | CPU id (MIDR/cache/ISA features) + `/proc/cpuinfo` + PMU cycles | Cortex-A72 detected |
| `feat/blk-irq` | Interrupt-driven block device — proves the GIC SPI fix generalizes | blk SPI counted |
| `feat/shell-top` | `top` dashboard (uptime/mem/tasks/irqs) | live overview |
| `feat/sched-stats` | Per-task CPU-time accounting (TICKS in `ps`) | idle accrues ticks |
| `feat/net-ping-stats` | `ping [count]` with RTT + loss | 3/3, ~127 µs |
| `feat/reboot-psci` | Real reboot via PSCI `SYSTEM_RESET` | banner prints twice |
| `feat/shell-rand` | `rand [n]` via VirtIO RNG | distinct bytes |
| `feat/sched-ctxt` | Context-switch counter in `ps` | count grows |
| `feat/elf-argv` | `argv` passing to ELF programs (aarch64 startup stack) | echoes its arg |
| `feat/syscall-irq-unmask` | Unmask IRQs in syscall dispatch (correctness) | no regression |
| `feat/smp` | Secondary-core bringup via PSCI `CPU_ON` | core1 MPIDR reported |
| `feat/rtc` | PL031 RTC + `date` (epoch→UTC) | matches host time |
| `feat/shell-cp` | `cp` (VFS read → FAT32 create) | copy round-trips |
| `feat/net-dns` | UDP DNS resolver + `resolve` | example.com → real IP |
| `feat/shell-memtest` | Allocator stress self-test | PASS, heap reclaims |
| `feat/exception-stats` | Per-class trap counters + `traps` | svc/irq counts |
| `feat/shell-sysinfo` | `sysinfo` summary | combined overview |
| `feat/sys-getrandom` | `SYS_GETRANDOM` syscall + user hex ELF | distinct random hex |
| `feat/smp-heartbeat` | Secondary runs a continuous heartbeat | counter advances |
| `feat/net-arp` | General ARP cache + `arp [ip]` | resolves gateway/DNS MAC |
| `feat/heap-stats` | Alloc/free/peak counters + `heapstat` | 90/45/peak shown |
| `feat/shell-history` | Command history + up/down-arrow recall | up-arrow re-runs |
| `feat/shell-tabcomplete` | TAB completion of builtins | `ver`+TAB → version |
| `feat/shell-blk` | Raw sector I/O (`blkdump`/`blkwrite` via /dev/blk) | sector round-trip |
| `feat/shell-wc` | `wc` line/word/byte count | 2 14 73 on HELLO.TXT |
| `feat/shell-stat` | `stat` vnode metadata | type/size/cluster |
| `feat/net-tcp` | **Minimal TCP client + `http <host>`** — fetches real pages | HTTP/1.1 200 OK |
| `feat/smp-mmu` | **Secondary enters higher half with MMU + atomics + vectors** | concurrent kprintln |
| `feat/smp-print-lock` | IRQ-safe cross-core print lock (atomic lines) | clean SMP output |
| `feat/smp-tasks` | **Cooperative multitasking on the secondary core** | 2 core-1 tasks advance |

### Headline results
- **TCP**: `http example.com` opens a real TCP connection through slirp and
  prints `HTTP/1.1 200 OK / Content-Type: text/html`.
- **DNS**: `resolve example.com` → a real Cloudflare IP.
- **SMP**: a second core enters the higher half with the MMU on, runs real
  kernel code (verified by *interleaved* concurrent `kprintln` before the print
  lock), uses atomics, and runs **two of its own tasks** via `context_switch`.

---

## 8. Networking stack (built incrementally)

ARP → IPv4 → ICMP (ping) → DHCP (DISCOVER/OFFER/REQUEST) → DNS (UDP) →
ARP cache → **TCP** (handshake, GET, data, FIN). The kernel can resolve a
hostname and fetch a web page over a real TCP connection (via QEMU slirp,
which forwards to the host network).

---

## 9. SMP status

A working **foundation** for symmetric multiprocessing:
- `smp` — PSCI `CPU_ON` brings up core 1.
- `smp-mmu` — core 1 enables the MMU (reusing the primary's page tables),
  jumps to the higher half, installs exception vectors, and runs real high-VA
  kernel code with **atomics**.
- `smp-print-lock` — an IRQ-safe spinlock serializes `kprintln!` across cores.
- `smp-tasks` — core 1 runs **its own cooperative scheduler** over an idle
  context plus two tasks, switching via `context_switch`. Allocations happen
  only during the serialized bringup window (core 0 waits on `SECONDARY_UP`),
  so the unlocked PMM is never touched concurrently.

**Not yet done (remaining big-ticket):** *symmetric* scheduling where arbitrary
tasks migrate between cores. That needs a per-core `current`, a `SpinLock`-
protected shared run queue (migrating off the single-core `Racy` globals),
per-core GIC redistributor + timer for preemption, and IPIs for cross-core
wakeups — a focused effort best done with this foundation in place.

---

## 10. Deferred / known limitations

- **Symmetric SMP scheduling** (above) — only the foundation + cooperative
  per-core scheduling exist.
- **TCP** is a one-shot client (no retransmission, windowing, or listen).
- **Cosmetic**: under QEMU `-nographic`, the very first console input byte of a
  session can be eaten by the stdio mux (a harness artifact, not a kernel bug).
- No C user libc — user programs are written directly in aarch64 assembly
  against the raw syscall ABI.

---

## 11. How everything was verified

Every milestone and feature was run in QEMU and confirmed by grepping the serial
output for concrete evidence (e.g., `MMU TEST] TTBR1 Upper Half: PASS`,
`PING reply from 10.0.2.2 ttl=255`, `HTTP/1.1 200 OK`, `hello from a loaded
ELF64 binary`, the `fermi>` prompt). The CI workflow (`.github/workflows/ci.yml`)
codifies this: it builds the kernel + disk, boots in QEMU, and asserts the
kernel reached the MMU / driver / networking / ELF / shell milestones.

The discovery of *real bugs during bring-up* (the FP trap, the TCP checksum
buffer-reuse, the GICv3 SPI routing, the pre-MMU relocation hazard) is itself
evidence the implementation is genuinely exercised end-to-end, not stubbed.
