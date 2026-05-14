# Fermi OS

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel built from scratch in `C` and assembly, targeting QEMU's `virt` machine with a Cortex-A72 processor.


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
- **POSIX-style Syscalls** — Sequential numbering: `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_EXIT`, `SYS_YIELD`, `SYS_SLEEP`. I/O syscalls route through the current task's fd table

### PCI & VirtIO
- **PCI Express (ECAM)** — Brute-force bus enumeration, device discovery and caching, BAR assignment (32-bit and 64-bit MMIO), Memory Space + Bus Master enable via Command register
- **VirtIO PCI Transport** — Capability list walking (vendor-specific 0x09), common/notify/ISR/device config BAR+offset resolution, MMIO register access via named offsets (virtio spec 4.1.4.3)
- **Virtqueue (Split)** — Reusable split virtqueue module with descriptor table, available ring, used ring, `VIRT_TO_PHYS` DMA address conversion, submit/notify/poll API
- **VirtIO RNG Driver** — Full VirtIO device init sequence (reset → ack → driver → feature negotiation → FEATURES_OK → queue setup → DRIVER_OK), random byte generation via `virtio-rng-pci`
- **VirtIO Block Driver** — `virtio-blk-pci` device init, capacity readout from device config, and synchronous 512-byte sector `blk_read`/`blk_write` using chained descriptors (header + data + status) over the split virtqueue

### Filesystem & Devices
- **Virtual Filesystem (VFS)** — Unix-style vnode tree with path resolution (`.`, `..`, multi-slash tolerant). Per-vnode `file_operations` vtable (read/write) and `vnode_operations` vtable (lazy `lookup`). Supports char devices, block devices, directories, regular files
- **Per-Process File Descriptor Table** — `fd_table_t` allocated per task, freed on reap. fd 0/1/2 auto-opened to `/dev/console` (stdin/stdout/stderr). `fd_open`/`fd_read`/`fd_write`/`fd_close`/`fd_seek` (SEEK_SET/SEEK_CUR) dispatching through vnode ops
- **Built-in Char Devices** — `/dev/console` (UART read/write), `/dev/null` (discard/EOF), `/dev/zero` (zero-fill), `/dev/rng` (virtio-rng with bounce buffer for DMA)
- **Block Device Node** — `/dev/blk` exposing the virtio-blk disk with sector-aligned byte-offset read/write
- **FAT32 (VFS-backed)** — Mounted at `/mnt/fat32`. Lazy directory traversal: each `lookup` walks on-disk directory entries and creates a vnode on demand with per-vnode `(first_cluster, size)` state. `open`/`read` on regular files goes through the full VFS → fd → `file_operations.read` path

---

## Prerequisites
> Note: This project is being developed and tested on Mac M4 chip. There is a possibility that you might encounter environment setup errors on other platforms.

Install [docker](https://www.docker.com) on your host machine.

```bash
git clone https://github.com/rituparna-ui/fermi-os.git
cd fermi-os

docker run -d -it -v .:/root/fermi-os --name osdev ubuntu
```

Once the Docker container is up and running, start a shell in the container that was just created.
```bash
docker exec -it osdev bash
```

Inside the Docker container, install required dependencies.
```bash
apt update && apt upgrade

apt install make qemu-system gcc-aarch64-linux-gnu gdb-multiarch tmux mtools dosfstools
ln -sf aarch64-linux-gnu-as /usr/bin/as
```

## Building & Running

```bash
# Build the kernel ELF
make

# Build and run in QEMU (serial console)
make run

# Clean build artifacts
make clean
```

To exit QEMU: `Ctrl-A` then `X`

## Debugging

```bash
# Launch QEMU paused + GDB in a tmux split
make tmux
```

Or manually in two terminals:

```bash
# Terminal 1: QEMU waiting for debugger
make debug

# Terminal 2: GDB connecting to QEMU
make gdb
```

### Other Utilities

```bash
# Generate compile_commands.json for clangd / IDE support
make compile_commands.json

# Dump QEMU device tree source (DTS)
make dump_dts
```
