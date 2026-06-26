# Fermi OS

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel built from scratch in **pure Rust and assembly**, targeting QEMU's `virt` machine with a Cortex-A72 processor.

> This branch (`fermi-claude-rs`) is a complete, ground-up Rust
> re-implementation of the original C kernel, ported feature by feature
> following the original commit history. The build is pure Rust — `rustc`
> targeting `aarch64-unknown-none`, linked with the bundled `rust-lld`, with
> assembly assembled by LLVM's integrated assembler. **No GCC or GNU binutils
> required.** The original C sources remain available in git history.
>
> Every subsystem is boot-verified under QEMU: MMU/heap self-tests pass, the
> EL0 shell is interactive, DHCP acquires a lease and ICMP pings the gateway,
> FAT32 files read through the VFS, and demand-paged/kill-on-fault handling
> works. Module layout mirrors the original tree: `src/{arch,klib,mm,exception,
> sched,syscall,drivers,fs,user}`.
>
> It also doubles as a minimal **Type-1 (bare-metal) hypervisor**: when launched
> with QEMU's `virtualization=on`, Fermi boots at **EL2**, sets up stage-2
> translation, and runs *itself* as a stage-2-translated EL1 guest alongside a
> second isolated guest — with virtual interrupts, preemptive EL2 scheduling,
> per-guest context switching, and PSCI-based guest lifecycle. Without
> `virtualization=on` the image boots straight at EL1 as a plain kernel, so the
> EL2 layer is fully backwards-compatible. See **Hypervisor (EL2)** below.
> The EL2 layer (`src/hyp/`) was added in a second porting phase mirroring the
> C original's M1–M13 milestones.


---

## Features

### Boot & Memory
- **PL011 UART Driver** — Full serial I/O, hex/decimal/binary output and formatted print with `%s %d %u %x %p %b %c %%` format specifiers
- **Physical Memory Manager (PMM)** — Bitmap-based page allocator managing 8 GB of RAM, with single and contiguous multi-page allocation
- **MMU (Memory Management Unit)** — 3-level page tables (L0→L1→L2) with 2 MB blocks, 48-bit virtual address space, 4 KB granule
- **Higher-Half Kernel** — Kernel linked at Virt Memory Address `0xFFFF_0000_4000_0000` with physical Load Memory Address `0x4000_0000`. Dual address space with TTBR0 (user) and TTBR1 (kernel)
- **Kernel Heap** — First-fit allocator with block splitting, coalescing, double-free detection, and bounds checking (`kmalloc`/`kfree`)
- **Kernel Panic Handler** — System register dump and CPU halt on unrecoverable errors

### Exceptions & Interrupts
- **Exception Handling** — Full ARMv8-A vector table, trap frame save/restore, ESR decoding, register dump on fault
- **GICv3 Interrupt Controller** — Minimal GICv3 bringup with Distributor/Redistributor initialization, affinity routing, system register interface, IRQ acknowledge/EOI
- **ARM Generic Timer** — Configurable periodic tick (default 1 s) driving the scheduler, routed through GICv3 PPI

### Scheduling & Processes
- **Preemptive Scheduler** — Round-robin task scheduler with timer-driven preemption, per-task kernel stacks, context switching via callee-saved register save/restore, task creation/exit/reaping lifecycle, and a circular run queue
- **Task Sleep** — Tick-based voluntary sleep via `sleep_ms(ms)`, with per-task deadlines and automatic wakeup on timer IRQ (`sched_wake_sleepers`)
- **EL0 User-Space Tasks** — Full EL1→EL0 privilege separation via `eret`, per-task user text and stack mappings with proper permission bits (UXN, PXN, AP), separate kernel and user stacks, and a trampoline that sets `SP_EL0`/`ELR_EL1`/`SPSR_EL1` before dropping to user mode
- **Per-Task Address Spaces (TTBR0 Switching)** — Each task gets its own L0→L1→L2 user page tables allocated from the PMM; `context_switch` swaps `TTBR0_EL1` and performs `TLBI`/`DSB`/`ISB` on every task switch, with recursive page table teardown on task reap
- **System Call Interface (SVC)** — AAPCS64-based `svc #0` dispatch with `x8` as syscall number and `x0`–`x7` for arguments; return value written back via the trap frame
- **POSIX-style Syscalls** — `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_EXIT`, `SYS_YIELD`, `SYS_SLEEP`, `SYS_GETPID`, `SYS_LSEEK`, `SYS_UPTIME`, `SYS_NET_PING`, `SYS_KILL`. I/O syscalls route through the current task's fd table; the dispatch path unmasks IRQs so blocking syscalls don't starve other tasks. User pointers are range-checked against `[0, USER_STACK_TOP)` to close kernel-pointer-injection holes
- **EL0 Page-Fault Handling** — Data and Instruction aborts from a lower EL kill *only* the offending task (logging pid/ELR/FAR/ESR) and continue scheduling; aborts from EL1 still trip a full `kernel_panic` since they indicate a real kernel bug
- **Kernel-Mode (EL1) Tasks** — `sched_create_kernel_task` provides a parallel scheduler path with its own `kernel_task_trampoline` (no `eret`, no TTBR0 swap). Used by the in-tree `netd` daemon

### PCI & VirtIO
- **PCI Express (ECAM)** — Brute-force bus enumeration, device discovery and caching, BAR assignment (32-bit and 64-bit MMIO), Memory Space + Bus Master enable via Command register
- **VirtIO PCI Transport** — Capability list walking (vendor-specific 0x09), common/notify/ISR/device config BAR+offset resolution, MMIO register access via named offsets (virtio spec 4.1.4.3)
- **Virtqueue (Split)** — Reusable split virtqueue module with descriptor table, available ring, used ring, `VIRT_TO_PHYS` DMA address conversion, submit/notify/poll API
- **VirtIO RNG Driver** — Full VirtIO device init sequence (reset → ack → driver → feature negotiation → FEATURES_OK → queue setup → DRIVER_OK), random byte generation via `virtio-rng-pci`
- **VirtIO Block Driver** — `virtio-blk-pci` device init, capacity readout from device config, and synchronous 512-byte sector `blk_read`/`blk_write` using chained descriptors (header + data + status) over the split virtqueue
- **VirtIO Net Driver** — `virtio-net-pci` device init (modern dev id `0x1041`, `VIRTIO_F_VERSION_1` required), MAC + link-status readout, RX queue pre-fill (8 1 600-byte buffers), `net_tx`/`net_rx_poll` synchronous APIs, and per-direction packet counters

### Networking
- **Layer 2 — Ethernet** — Hand-built ARP requests/replies. Boot path sends a broadcast ARP for the slirp gateway, parses the reply, and caches the resulting `gateway_mac`
- **Layer 3 — IPv4 + ICMP echo** — RFC 1071 internet checksum helper, IPv4 header builder, and ICMP echo request that drives a full L3 round-trip to QEMU's slirp gateway (`10.0.2.2`). Verified: ARP → IPv4 → ICMP echo reply with TTL
- **netd kernel daemon** — Periodic background pinger running at EL1: drains incoming RX, sends an ICMP echo every 5 s, and prints reply latency in ticks
- **`SYS_NET_PING`** — EL0-callable ICMP echo so user-space tasks (notably the shell's `ping` command) can fire pings without leaving the syscall ABI

### Filesystem & Devices
- **Virtual Filesystem (VFS)** — Unix-style vnode tree with path resolution (`.`, `..`, multi-slash tolerant). Per-vnode `file_operations` vtable (read/write) and `vnode_operations` vtable (lazy `lookup`). Supports char devices, block devices, directories, regular files
- **Per-Process File Descriptor Table** — `fd_table_t` allocated per task, freed on reap. fd 0/1/2 auto-opened to `/dev/console` (stdin/stdout/stderr). `fd_open`/`fd_read`/`fd_write`/`fd_close`/`fd_seek` (SEEK_SET/SEEK_CUR) dispatching through vnode ops
- **Built-in Char Devices** — `/dev/console` (UART read/write), `/dev/null` (discard/EOF), `/dev/zero` (zero-fill), `/dev/rng` (virtio-rng with bounce buffer for DMA)
- **Block Device Node** — `/dev/blk` exposing the virtio-blk disk with sector-aligned byte-offset read/write
- **FAT32 (VFS-backed)** — Mounted at `/mnt/fat32`. Lazy directory traversal: each `lookup` walks on-disk directory entries and creates a vnode on demand with per-vnode `(first_cluster, size)` state. `open`/`read` on regular files goes through the full VFS → fd → `file_operations.read` path
- **`/proc` synthetic filesystem** — Mounted at `/proc`, regenerates content per-read from live kernel state. Files: `/proc/uptime`, `/proc/meminfo` (PMM + heap), `/proc/tasks` (run-queue snapshot with state names), `/proc/interrupts` (per-INTID counts via the GIC dispatch hook), `/proc/netinfo` (MAC, link, IP, gateway MAC, packet counters), `/proc/cmdline`, `/proc/version`

### EL0 Shell (interactive)
- **`task_shell`** — An EL0 task that loops reading lines from `/dev/console` (with backspace/DEL editing and echo) and dispatches built-ins. Pure user-space — talks to the kernel only via `svc`. Built-ins: `help`, `pid`, `uptime`, `ps`, `free`, `ifconfig`, `irqs`, `version`, `cat <path>`, `kill <pid>`, `top` (5× refresh tasks/mem/net), `ping`, `sleep <ms>`, `clear`, `exit`

### Hypervisor (EL2 Type-1 VMM)
> Requires QEMU `virt,gic-version=3,virtualization=on` (and `-m 10G` for the Linux slot). Without `virtualization=on` the image boots straight at EL1 as a plain kernel — the EL2 layer is skipped cleanly, and the standard smoke-test still passes. Stage-2 translation needs **QEMU ≥ 8** (the host's 3.1.0 faults at stage-2 level 0 — an emulator limitation); `ci/hyp-smoke-test.sh` runs it under QEMU 8.x.
- **EL2 bring-up** — `boot.S` detects entry at EL2 via `CurrentEL`, configures the hypervisor, then `eret`s to EL1. A dedicated EL2 vector table (`VBAR_EL2`) + private EL2 stack handle all guest traps; the EL2-vs-EL1 entry decision is threaded through a callee-saved register so it survives `zero_bss`
- **Stage-2 translation** — Per-guest second-stage (IPA→PA) page tables via `VTTBR_EL2`/`VTCR_EL2` with distinct VMIDs (no TLB flush on switch). The primary guest gets a 1 TiB identity map (1 GiB blocks); the hypervisor's own RAM is split to 4 KiB granularity and **unmapped** from every guest
- **Hypervisor isolation** — A guest read/write to hypervisor-private memory faults to EL2, is reported, poisoned, and stepped over — the guest can never see or corrupt VMM state
- **Hypercall ABI** — SMCCC-style `HVC` interface (function ID in `x0`, args `x1`–`x3`, result in `x0`): `VERSION`, `PUTC` (paravirt console), `PING`, `YIELD`, `VM_INFO`, `HYP_BASE`, plus introspection (`VM_COUNT`/`VM_STAT`)
- **Trap-and-emulate** — `HCR_EL2.TID3` can trap guest `ID_AA64*` reads; the handler decodes `ESR_EL2` and emulates them, stepping the guest PC past the trapped instruction
- **Virtual interrupts (vGIC)** — Physical IRQs are routed to EL2 (`HCR_EL2.IMO`) and re-injected as hardware-linked virtual interrupts via the GICv3 list registers (`ICH_LR<n>_EL2`, HW=1), so the guest's unmodified IRQ handler runs on the virtual CPU interface and its EOI auto-deactivates the physical interrupt. Interrupts are routed to their owning vCPU
- **Preemptive scheduler** — The EL2 physical timer (`CNTHP_EL2`, PPI 26) drives a 100 ms quantum; on each tick the hypervisor world-switches between vCPUs round-robin. No guest cooperation required. Per-guest EL1 sysregs, vGIC state, and full FP/SIMD (q0–q31) are saved/restored across switches
- **Guest lifecycle** — `/proc/vms` exposes live vCPU state over the hypercall ABI; a guest can power itself off via PSCI `SYSTEM_OFF`, after which the hypervisor reaps the vCPU
- **Linux-guest slot** — A 1 GiB guest RAM window at IPA `0x40000000` backed by host-invisible high RAM, emulated GICv3 distributor/redistributor MMIO, and CNTV virtual-timer routing. `hyp_create_linux_guest` auto-detects a staged arm64 Linux Image by its header magic: if present it enters the Image per the boot protocol (PC = Image, `x0` = DTB); if absent — the default, since the Image/initramfs are external (uncommitted) assets — it falls back to a self-contained bring-up stub so the slot is a working second guest out of the box. See [`docs/PORT-NOTES.md`](docs/PORT-NOTES.md) §6 for how to stage real assets

---

## Prerequisites

The build is **pure Rust** — no GCC or GNU binutils required. Assembly (`.S`
files) is assembled by LLVM's integrated assembler, and linking uses the
toolchain-bundled `rust-lld`.

```bash
# Rust toolchain + the bare-metal aarch64 target
rustup target add aarch64-unknown-none

# QEMU for running the kernel
#   macOS:  brew install qemu
#   Debian: apt install qemu-system-arm
```

## Building & Running

```bash
# Build the kernel ELF
cargo build

# Build and run in QEMU (serial console)
cargo run

# Release build
cargo build --release
```

The QEMU invocation lives in `run.sh` (wired up as the Cargo `runner`).

To exit QEMU: `Ctrl-A` then `X`

To boot it **as a hypervisor** (EL2), add `virtualization=on` and bump RAM so
the Linux-guest slot at 9 GiB fits (needs QEMU ≥ 8 for stage-2):

```bash
qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on \
    -cpu cortex-a72 -m 10G -nographic \
    -kernel target/aarch64-unknown-none/debug/kernel
```

## Testing

A headless boot smoke-test builds a FAT32 disk, boots the kernel under QEMU,
and asserts the key subsystem milestones (MMU/heap self-tests, VirtIO devices,
DHCP lease, ICMP ping, FAT32 read) appear and nothing panics:

```bash
cargo build
./ci/smoke-test.sh
```

A second smoke-test boots the kernel **as a hypervisor** (`virtualization=on`,
entering at EL2) and asserts the EL2 milestones — stage-2 enabled, the isolation
boundary blocks a guest read of hyp memory, a virtual IRQ is injected, the
second guest is created and scheduled, `/proc/vms` reports both vCPUs — then the
full EL1 guest reaching `Ready`:

```bash
./ci/hyp-smoke-test.sh
```

Stage-2 translation needs QEMU ≥ 8; the script uses the host QEMU if it's new
enough, else the `osdev:dev` Docker image, else SKIPs cleanly.

CI (`.github/workflows/ci.yml`) runs `clippy -D warnings`, builds debug +
release, runs the EL1 smoke-test against both, and runs the EL2 hypervisor
smoke-test on every push/PR.

## Documentation

- [`docs/BUILD-SUMMARY.md`](docs/BUILD-SUMMARY.md) — what was built on this
  branch (kernel + hypervisor), the commit-by-commit map, module layout, and how
  this branch relates to the other branches/worktrees in the repo.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — subsystem map, boot flow,
  memory layout, the frozen ABI contracts (trap frame, syscall numbers, PTE
  format), the concurrency model, and the test matrix.
- [`docs/PORT-NOTES.md`](docs/PORT-NOTES.md) — the C→Rust port plan, build order,
  and risk register.
- [`docs/PROJECT-JOURNAL.md`](docs/PROJECT-JOURNAL.md) — the full record of the
  C→Rust port: strategy, every phase, decisions and rationale, bugs found, and
  the hardening/feature work that followed.

## Debugging

Run QEMU paused with a GDB stub on `:1234`:

```bash
qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a72 -m 8G \
    -nographic -kernel target/aarch64-unknown-none/debug/kernel -s -S
```

Then connect with a GDB that understands aarch64 (e.g. `gdb-multiarch`):

```bash
gdb-multiarch target/aarch64-unknown-none/debug/kernel \
    -ex "target remote :1234" -ex "layout split"
```
