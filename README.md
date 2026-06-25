# Fermi OS (Rust)

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel written in **pure Rust +
aarch64 assembly**, targeting QEMU's `virt` machine with a Cortex-A72. It is a
ground-up port of the original C+assembly Fermi OS, rebuilt subsystem-by-
subsystem in `no_std` Rust.

Only five hand-written assembly files remain (everything else is Rust):

| File | Purpose |
|------|---------|
| `src/boot.S` | Reset entry: stack, FP enable, BSS zero, higher-half jump |
| `src/exception/vector.S` | EL1 exception vector table + trap-frame save/restore |
| `src/sched/switch.S` | Context switch (GPR+FP) + EL0/EL1 task trampolines |
| `src/sched/user_prog.S` | Embedded position-independent EL0 demo program |
| `src/smp.S` | Secondary-core reset trampoline (FP, stack, MMU bring-up) |

> This is the **consolidated `integration` branch**: the base C→Rust port plus
> ~39 verified feature branches merged into one tree (SMP, the full networking
> stack, the FAT32 write suite, RTC, and a rich shell).

## Features

- **Boot & memory** — PL011 UART, bitmap Physical Memory Manager (8 GiB),
  3-level MMU (4 KiB granule, 48-bit VA), **higher-half kernel** running from
  TTBR1 with per-task TTBR0, first-fit kernel heap behind Rust's
  `#[global_allocator]` (so `alloc::{Vec, Box, String}` work kernel-wide).
- **Exceptions & interrupts** — full ARMv8-A vector table with ESR/DFSC
  decode, kernel panic with register dump, GICv3 bringup, ARM generic timer
  (drift-free 10 ms tick), per-INTID counters.
- **Scheduling & processes** — preemptive round-robin scheduler with per-task
  CPU-time and context-switch accounting, per-task kernel stacks, tick-based
  `sleep`, EL1 kernel tasks and **EL0 user tasks** with per-task address spaces;
  SVC syscall dispatch
  (`read/write/open/close/lseek/exit/yield/sleep/getpid/uptime/net_ping/kill/fork/exec/getrandom`).
- **SMP** — a secondary core is brought up via PSCI `CPU_ON`, enables the MMU
  (shared page tables) and runs in the higher half with its own GIC CPU
  interface, exception vectors, and per-core generic timer. It runs its **own
  preemptive scheduler** (idle + tasks, timer-driven), and a shared
  `SpinLock`-protected work queue is drained concurrently by **both cores**
  (verified: 8000 jobs, exact checksum, no loss/duplication). Cross-core
  `kprintln!` is serialized by an IRQ-safe print lock.
- **PCI & VirtIO** — PCI ECAM enumeration + BAR assignment, modern VirtIO PCI
  transport, split virtqueue, and drivers for **RNG, block, net, console, and
  balloon** (block I/O serialized by an IRQ-safe lock for SMP safety).
- **Networking** — ARP (+ cache), IPv4, ICMP echo (verified ping round-trip),
  a DHCP client, a UDP **DNS** resolver, a minimal **TCP** client that fetches
  real web pages (`http <host>` → `HTTP/1.1 200 OK`), an **SNTP** client
  (`ntp` → network time), and a `netd` background pinger.
- **Filesystem** — Unix-style VFS (vnode tree, path resolution), per-process
  fd table (fd 0/1/2 → `/dev/console`), char/block device nodes
  (`/dev/{console,null,zero,rng,vcons,blk}`), **FAT32** (read, create, delete,
  rename) mounted at `/mnt/fat32` with a `df` free-space view, and a `/proc`
  synthetic filesystem
  (`uptime/meminfo/tasks/interrupts/netinfo/cmdline/version/cpuinfo`).
- **RTC** — PL031 real-time clock with a `date` command (epoch → UTC).
- **Shell** — an interactive line editor (command history + arrow-key recall,
  TAB completion) with builtins for diagnostics (`ps top sysinfo free irqs
  traps cpuinfo heapstat memtest smp smptest`), networking (`ifconfig ping arp
  resolve http ntp`), files (`ls cat grep wc stat write cp mv rm hexdump df
  run`), raw block I/O (`blkdump blkwrite`), and process control (`sleep kill
  echo rand reboot clear balloon vlog`).

## Prerequisites

- Rust (stable) with the bare-metal target:
  ```bash
  rustup target add aarch64-unknown-none
  ```
- `qemu-system-aarch64`, and `mkfs.fat` + `mtools` (to build the FAT32 disk).

## Building & running

```bash
make disk      # build build/disk.img (FAT32 + HELLO.TXT) — once
make run       # cargo run: boots in QEMU (serial console)
make build     # cargo build only
make clean      # cargo clean
```

`make run` (or `cargo run`) launches QEMU with virtio rng/blk/net/console/
balloon devices attached and drops you at the `fermi>` shell prompt.

To exit QEMU: `Ctrl-A` then `X`.

## ELF programs

A standalone aarch64 `ET_EXEC` user binary (`user/hello.S`) is built into the
FAT32 image as `HELLO.ELF`. The kernel's ELF64 loader maps its PT_LOAD segments
(W^X) into a fresh user address space and runs it at EL0 — both automatically at
boot and interactively via the shell's `run /mnt/fat32/HELLO.ELF`.

Process control: `fork()` deep-copies the calling task into a new address
space (child resumes in the same SVC with `x0 = 0`); `exec()` replaces the
current image with an ELF loaded from the filesystem. The embedded EL0 demo
forks and the child `exec()`s `HELLO.ELF`.

## Notes

- User programs are written directly in aarch64 assembly against the raw
  syscall ABI (there is no C user libc).
- FAT32 supports read, create, delete, and rename of root-level files.
- User stacks grow on demand: a fault in the stack-growth zone maps a fresh
  page (up to 80 KiB) and resumes the faulting instruction.
