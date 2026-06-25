# Fermi OS (Rust)

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel written in **pure Rust +
aarch64 assembly**, targeting QEMU's `virt` machine with a Cortex-A72. It is a
ground-up port of the original C+assembly Fermi OS, rebuilt subsystem-by-
subsystem in `no_std` Rust.

Only four hand-written assembly files remain (everything else is Rust):

| File | Purpose |
|------|---------|
| `src/boot.S` | Reset entry: stack, FP enable, BSS zero, higher-half jump |
| `src/exception/vector.S` | EL1 exception vector table + trap-frame save/restore |
| `src/sched/switch.S` | Context switch (GPR+FP) + EL0/EL1 task trampolines |
| `src/sched/user_prog.S` | Embedded position-independent EL0 demo program |

## Features

- **Boot & memory** — PL011 UART, bitmap Physical Memory Manager (8 GiB),
  3-level MMU (4 KiB granule, 48-bit VA), **higher-half kernel** running from
  TTBR1 with per-task TTBR0, first-fit kernel heap behind Rust's
  `#[global_allocator]` (so `alloc::{Vec, Box, String}` work kernel-wide).
- **Exceptions & interrupts** — full ARMv8-A vector table with ESR/DFSC
  decode, kernel panic with register dump, GICv3 bringup, ARM generic timer
  (drift-free 10 ms tick), per-INTID counters.
- **Scheduling & processes** — preemptive round-robin scheduler, per-task
  kernel stacks, tick-based `sleep`, EL1 kernel tasks and **EL0 user tasks**
  with per-task address spaces; SVC syscall dispatch
  (`read/write/open/close/lseek/exit/yield/sleep/getpid/uptime/net_ping/kill`).
- **PCI & VirtIO** — PCI ECAM enumeration + BAR assignment, modern VirtIO PCI
  transport, split virtqueue, and drivers for **RNG, block, net, console, and
  balloon**.
- **Networking** — ARP, IPv4 + ICMP echo (verified ping round-trip to the
  slirp gateway), a minimal DHCP client, and a `netd` background pinger.
- **Filesystem** — Unix-style VFS (vnode tree, path resolution), per-process
  fd table (fd 0/1/2 → `/dev/console`), char/block device nodes
  (`/dev/{console,null,zero,rng,vcons,blk}`), read-only **FAT32** mounted at
  `/mnt/fat32`, and a `/proc` synthetic filesystem
  (`uptime/meminfo/tasks/interrupts/netinfo/cmdline/version`).
- **Shell** — an interactive shell with builtins: `help, uptime, version, ps,
  free, ifconfig, irqs, cat, ping, sleep, kill, echo, balloon, vlog, cpuinfo,
  clear, reboot`.

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

## Deferred from the original

- ELF64 loader + `fork`/`exec` (the kernel runs an embedded EL0 program rather
  than loading binaries from disk).
- FAT32 create/write (read path only).
- Demand-paged user-stack growth (a user stack fault currently kills the task).
