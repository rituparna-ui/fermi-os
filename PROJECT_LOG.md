# Fermi OS — C → Rust Port: Project Log

This document records **everything done in this effort and why**: the goal, the
porting strategy, every subsystem milestone, the bare-metal-Rust pitfalls hit
and fixed, the full set of feature branches, and how each piece was verified.

> Status at last update: pure-Rust kernel boots to an interactive `fermi>`
> shell; networking spans ARP/IPv4/ICMP/DHCP/DNS/UDP/**TCP**/**NTP**; SMP
> foundation runs a second core (MMU + atomics + its own task scheduler).
> ~35 branches pushed to `github.com/rituparna-ui/fermi-os`.

---

## 1. Goal

Convert the original **Fermi OS** — a bare-metal `aarch64` (ARMv8-A) kernel in
C + assembly for QEMU's `virt` machine (Cortex-A72) — into a **pure Rust +
aarch64 assembly** kernel, working through the original git history *subsystem
by subsystem*, building and **boot-testing in QEMU at every milestone**.

The original was ~150 KB of C across 143 commits. The result is **pure Rust
(~7k lines) + 4 hand-written assembly files**, zero C, booting to a shell.

---

## 2. Why this approach

- **Follow the commit history as a curriculum.** The original built up in a
  natural dependency order; porting in that order meant each layer's
  dependencies were already present and verifiable immediately.
- **Build + boot-test every milestone.** Each subsystem was compiled and run in
  QEMU, grepping serial output for proof. This surfaced the bare-metal-Rust
  pitfalls (§6) early, at the layer that caused them.
- **Assembly only where it must be**: boot, exception vectors, context switch,
  embedded user program, secondary-core entry.
- **Branch-based**: all work on `rust-port` and feature branches off it; the C
  history stays intact and recoverable on the original branch.

---

## 3. Toolchain / build / run

- `rustc` 1.85.0 stable, target **`aarch64-unknown-none`** (precompiled
  core/alloc — no `build-std`).
- `qemu-system-aarch64`, `clang` + `ld.lld` (user ELF binaries), `mkfs.fat` +
  `mtools` (FAT32 disk).
- `cargo build` → `target/aarch64-unknown-none/debug/kernel`.
- `make disk` → `build/disk.img` (FAT32 + seed files + user ELFs).
- `make run` / `cargo run` → QEMU with virtio rng/blk/net/console/balloon
  (and `-smp 2`) → `fermi>` shell.

**Critical QEMU detail:** virtio devices need `disable-legacy=on` to present
*modern* PCI device IDs (blk `0x1042`, net `0x1041`); otherwise they're
transitional (`0x1001`/`0x1000`) and the modern-only drivers won't bind.

---

## 4. Module layout (pure Rust)

```
src/
  main.rs        crate root: no_std/no_main, global_asm!(boot.S),
                 early_init() (pre-MMU) + kernel_main() (higher half),
                 panic handler, demo tasks (netd, EL0 user, shell)
  boot.S         reset: FP enable, physical stack/BSS, early_init, jump to high
  cpu.rs         mrs!/msr!, current_el, CPU id + PMU, PSCI system_reset
  mmio.rs        VA-offset-routed MMIO (0 pre-MMU, KERNEL_VA_OFFSET after)
  uart.rs        PL011 + Device-safe pre-MMU log helpers
  print.rs       core::fmt over UART; kprint!/kprintln! + IRQ-safe cross-core lock
  sync.rs        SpinLock<T> (post-MMU) and Racy<T> (pre-MMU, lock-free)
  panic.rs       kernel_panic: sysreg dump + halt
  strings.rs     BufWriter (ksnprintf!) + cstr helpers
  mm/pmm.rs      bitmap physical page allocator (8 GiB) + relocate_upper
  mm/mmu.rs      3-level tables, identity + higher-half, TTBR0/TTBR1, ASIDs,
                 user-table walker/mapper, secondary-core MMU enable
  mm/heap.rs     first-fit heap + #[global_allocator] + alloc stats
  exception.rs   trap-frame vectors, ESR/DFSC decode, dispatch, trap stats
    exception/vector.S  full GPR + ELR/SPSR/ESR/FAR trap frame
    exception/gic.rs    GICv3 bringup + SPI routing + per-INTID counters
    exception/timer.rs  ARM generic timer (drift-free CNTP_CVAL tick)
  sched/mod.rs   round-robin scheduler, Task, fork/exec, sleep, kill, stats,
                 make_kernel_task / raw_context_switch (for SMP)
    sched/switch.S    context_switch (GPR+FP), EL0/EL1 trampolines, fork_return
    sched/user_prog.S embedded EL0 program (stack-grow probe, fork, child exec)
  syscall.rs     SVC dispatch: read/write/open/close/lseek/exit/yield/sleep/
                 getpid/uptime/net_ping/kill/fork/exec/getrandom
  pci.rs         PCI ECAM enumeration, BAR assignment, INTx -> GIC INTID
  virtio/mod.rs  VirtIO PCI transport: caps, handshake, status
    virtio/virtqueue.rs  split virtqueue
    virtio/{rng,blk,net,console,balloon}.rs  the five drivers
  net.rs         ARP+cache, IPv4, ICMP, DHCP, DNS, UDP, TCP, NTP, netd
  fs/vfs.rs      vnode tree, path resolution, fd table, device dispatch
    fs/devices.rs   /dev/{console,null,zero,rng,vcons,blk}
    fs/fat32.rs     FAT32 read + create + dir-list + usage, VFS lazy lookup
    fs/proc.rs      /proc synthetic fs
  shell.rs       interactive shell: history, tab-complete, ~35 builtins
  smp.rs / smp.S secondary-core bringup + MMU enable + cooperative scheduler
```

---

## 5. Milestones (what & why) — branch `rust-port`

Each committed separately and boot-verified.

1. **Scaffold + UART** — no_std cargo kernel, boot.S, PL011 + MMIO, panic.
2. **Exception level** — mrs!/msr!, read/print CurrentEL.
3. **PMM** — bitmap allocator over 8 GiB (single + contiguous).
4. **MMU + higher-half** — 3-level tables, TTBR0 identity + TTBR1 high, enable,
   then relink at `0xFFFF000040000000` and jump the kernel to the upper half so
   TTBR0 is free for per-task user mappings (basis for EL0 userspace).
5. **Heap** — first-fit + `#[global_allocator]` (Vec/Box/String work).
6. **Exceptions** — trap-frame vectors, ESR/DFSC decode, kernel panic.
7. **GICv3 + timer** — interrupt controller + drift-free 10 ms tick.
8. **strings/printf** — `ksnprintf!` + cstr helpers (core::fmt = uart_printf).
9. **Scheduler** — preemptive round-robin, sleep, FP-saving context switch.
10. **Syscalls + EL0** — TTBR0 swap, eret trampoline, SVC dispatch, EL0 prog.
11. **PCI + VirtIO + virtqueue** — verified by RNG DMA.
12. **VirtIO drivers** — rng, blk, net, console, balloon.
13. **VFS + FAT32 + devices + fd table** — fd 0/1/2 → /dev/console.
14. **Networking** — ARP, IPv4, ICMP ping, DHCP, netd.
15. **/proc** — synthetic fs from live state.
16. **ELF64 + fork + exec** — load from FAT32, run at EL0; fork copies the
    address space; exec replaces the image.
17. **Shell** — interactive line editor + builtins.
18. **Cleanup + CI** — removed all C, README/Makefile, QEMU boot smoke test.

---

## 6. Bare-metal-Rust pitfalls hit (and fixed)

Real bugs found by testing — things the C original never faced:

- **Atomics fault pre-MMU.** Exclusive LDXR/STXR (backing
  `compare_exchange`/`SpinLock`) is illegal on Device memory → use a lock-free
  `Racy<T>` before the MMU; `SpinLock` after.
- **`core::fmt` faults pre-MMU.** Unaligned 2-byte digit copies fault on Device
  memory → simple `uart` logging before the MMU; `kprintln!` after.
- **FP/SIMD trap (ESR EC=0x7).** LLVM uses SIMD in core::fmt/memcpy →
  enable `CPACR_EL1.FPEN` in `boot.S` before any Rust. (Cause of early "hangs".)
- **Pre-MMU absolute relocations.** `match`-returning-`&str` builds a high-VA
  pointer table that faults pre-MMU; inline `&str` literals are PC-relative
  (→ physical) and fine.
- **context_switch must save d8–d15** (kernel uses FP via core::fmt).
- **TCP checksum + buffer reuse** — frame buffer's checksum field wasn't zeroed
  before recompute; and slirp needs an explicit ACK to finalize the handshake.
- **GICv3 SPI delivery** — SPIs need IGROUPR/IPRIORITYR/IROUTER configured;
  the initial bringup only did PPIs, so no device IRQ could fire.

---

## 7. Feature branches (all pushed, boot-verified)

`rust-port` is the base. `net-dns`→`net-tcp`/`net-ntp` and the `smp-*` chain are
stacked (one depends on another); the rest branch directly off `rust-port`.

| Branch | What & why | Verified by |
|---|---|---|
| `rust-port` | Complete C→Rust port + IRQ-driven net + CI (base) | full boot-to-shell |
| `feat/shell-fileops` | `write` (FAT32 create) + `hexdump` | write→cat→hexdump |
| `feat/proc-cpuinfo` | CPU id + `/proc/cpuinfo` + PMU cycles | Cortex-A72 |
| `feat/blk-irq` | Interrupt-driven block device | blk SPI counted |
| `feat/shell-top` | `top` dashboard | live overview |
| `feat/sched-stats` | Per-task CPU ticks in `ps` | idle accrues ticks |
| `feat/net-ping-stats` | `ping [count]` RTT + loss | 3/3, ~127 µs |
| `feat/reboot-psci` | Real reboot (PSCI SYSTEM_RESET) | banner twice |
| `feat/shell-rand` | `rand [n]` via VirtIO RNG | distinct bytes |
| `feat/sched-ctxt` | Context-switch counter | count grows |
| `feat/elf-argv` | `argv` to ELF programs | echoes arg |
| `feat/syscall-irq-unmask` | Unmask IRQs in syscall dispatch | no regression |
| `feat/smp` | Secondary bringup (PSCI CPU_ON) | core1 MPIDR |
| `feat/rtc` | PL031 RTC + `date` | matches host time |
| `feat/shell-cp` | `cp` (VFS read → FAT32 create) | round-trips |
| `feat/net-dns` | UDP DNS resolver + `resolve` | real IP |
| `feat/shell-memtest` | Allocator stress test | PASS, reclaims |
| `feat/exception-stats` | Per-class trap counters + `traps` | svc/irq counts |
| `feat/shell-sysinfo` | `sysinfo` summary | combined view |
| `feat/sys-getrandom` | `SYS_GETRANDOM` + user hex ELF | distinct hex |
| `feat/smp-heartbeat` | Secondary continuous heartbeat | counter advances |
| `feat/net-arp` | ARP cache + `arp [ip]` | resolves MACs |
| `feat/heap-stats` | Alloc/free/peak + `heapstat` | 90/45/peak |
| `feat/shell-history` | History + arrow-key recall | up-arrow re-runs |
| `feat/shell-tabcomplete` | TAB completes builtins | ver+TAB→version |
| `feat/shell-blk` | Raw sector I/O (blkdump/blkwrite) | sector round-trip |
| `feat/shell-wc` | `wc` line/word/byte | 2 14 73 |
| `feat/shell-stat` | `stat` vnode metadata | type/size/cluster |
| `feat/net-tcp` | **TCP client + `http`** (fetches real pages) | HTTP/1.1 200 OK |
| `feat/smp-mmu` | **Secondary in higher half (MMU+atomics+vectors)** | concurrent kprintln |
| `feat/smp-print-lock` | IRQ-safe cross-core print lock (atomic lines) | clean SMP output |
| `feat/smp-tasks` | **Cooperative multitasking on core 1** | 2 core-1 tasks advance |
| `feat/shell-df` | Disk capacity + FAT32 usage (`df`) | 16MiB; 8/4084 clusters |
| `feat/net-ntp` | **SNTP client + `ntp`** (network time) | matches real time |
| `feat/shell-uname` | `uname [-a]` | system id |
| `docs/project-log` | This document | — |

### Headline verified results
- **TCP**: `http example.com` → `HTTP/1.1 200 OK / Content-Type: text/html` over
  a real TCP connection through slirp.
- **DNS**: `resolve example.com` → a real Cloudflare IP.
- **NTP**: `ntp` → `time.google.com` network time matching the host clock.
- **SMP**: a second core enters the higher half (MMU on), uses atomics, and runs
  **two of its own tasks** via `context_switch` (concurrent with core 0's
  shell). Cross-core `kprintln` interleaving (before the print lock) was direct
  evidence of true concurrent execution.

---

## 8. Networking stack (built incrementally)

ARP (+ cache) → IPv4 → ICMP (ping) → DHCP → DNS (UDP) → **TCP** (handshake,
GET, data, FIN — fetches web pages) → **NTP** (SNTP, network time). All outbound
traffic goes through QEMU slirp, which forwards to the host network.

---

## 9. SMP status

A working **foundation** for symmetric multiprocessing:
- `smp` — PSCI `CPU_ON` brings up core 1.
- `smp-mmu` — core 1 enables the MMU (reusing the primary's tables), jumps to
  the higher half, installs exception vectors, runs real high-VA code + atomics.
- `smp-print-lock` — IRQ-safe spinlock serializes `kprintln!` across cores.
- `smp-tasks` — core 1 runs **its own cooperative scheduler** (idle + 2 tasks)
  via `context_switch`. All allocation happens during the serialized bringup
  window (core 0 waits on `SECONDARY_UP`), so the unlocked PMM isn't touched
  concurrently; afterwards core 1 only does atomic work.

**Remaining big-ticket:** *symmetric* scheduling with task migration between
cores — needs a per-core `current`, a `SpinLock`-protected shared run queue
(migrating off the single-core `Racy` globals), per-core GIC redistributor +
timer for preemption, and IPIs for cross-core wakeups. A focused effort, best
done with this foundation in place.

---

## 10. Deferred / known limitations

- **Symmetric SMP scheduling** (above).
- **TCP** is a one-shot client (no retransmission/windowing/listen).
- **NTP** default host is `time.google.com` (the pool.ntp.org anycast didn't
  respond through slirp); outbound UDP to arbitrary ports may be filtered in
  some environments.
- **Cosmetic**: under QEMU `-nographic`, the first console input byte of a
  session can be eaten by the stdio mux (a harness artifact, not a kernel bug).
- No C user libc — user programs are aarch64 assembly against the raw syscall ABI.

---

## 11. How everything was verified

Every milestone/feature was run in QEMU and confirmed by grepping serial output
for concrete evidence (`MMU TEST] TTBR1 Upper Half: PASS`, `PING reply ... ttl=255`,
`HTTP/1.1 200 OK`, `network time = ...`, `hello from a loaded ELF64`, the
`fermi>` prompt, two-core task counters advancing). CI (`.github/workflows/
ci.yml`) builds the kernel + disk, boots in QEMU, and asserts the MMU / driver /
networking / ELF / shell milestones.

The discovery of *real bugs during bring-up* (FP trap, TCP checksum buffer
reuse, GICv3 SPI routing, pre-MMU relocation hazard, FAT32 16-bit sector count)
is itself evidence the implementation is genuinely exercised end-to-end.

---

## 12. Suggested integration order

1. Merge `rust-port` first (carries the GICv3 SPI-delivery fix the others rely on).
2. Then independent feature branches in any order.
3. Stacked chains: `net-dns` → `net-tcp` / `net-ntp`; `smp` → `smp-heartbeat`
   → `smp-mmu` → `smp-print-lock` → `smp-tasks`.
4. Trivial conflicts only: overlapping shell `dispatch` arms / help text, the
   `ps` renderer, and the `ProcKind` enum (`proc-cpuinfo` adds a variant).
