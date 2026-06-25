# Fermi OS — Architecture

Fermi OS is a bare-metal `aarch64` (ARMv8-A) kernel in pure Rust + assembly,
targeting QEMU's `virt` machine with a Cortex-A72. This document maps the
subsystems, the boot flow, the memory layout, and the frozen ABI contracts.

For the historical port plan, risk register, and the original-C subsystem
contracts, see [`PORT-NOTES.md`](PORT-NOTES.md).

---

## Toolchain

Pure Rust — no GCC or GNU binutils:

- `rustc` targets `aarch64-unknown-none` (precompiled `core`/`alloc`).
- Linking uses the toolchain-bundled `rust-lld` (`-C link-arg=-Tlinker.ld`).
- Assembly lives in `.S` files pulled in with `global_asm!(include_str!(...))`
  and is assembled by LLVM's integrated assembler.
- `#![no_std]`, `#![no_main]`, `panic = "abort"`, `#![deny(unsafe_op_in_unsafe_fn)]`.

Build: `cargo build` (debug) / `cargo build --release`. Run: `cargo run` (QEMU
via `run.sh`). Test: `./ci/smoke-test.sh`.

---

## Module map (`src/`)

| Path | Responsibility |
|------|----------------|
| `main.rs` | `early_init` (pre-MMU) + `kmain` (upper-half) boot orchestration; demo tasks; `netd`; churn test |
| `panic.rs` | `kernel_panic` (register dump + halt) and the `#[panic_handler]` |
| `arch/boot.S` | `_start`: park secondaries, set SP, zero BSS, call `early_init`, jump to upper-half `kmain` |
| `arch/sysreg.rs` | `mrs!`/`msr!` inline-asm system-register access |
| `arch/cpu.rs` | exception-level read, FP/SIMD enable, `dsb_sy`, CPU id + PMU (`cpuinfo`), PSCI reboot |
| `klib/uart.rs` | PL011 UART driver + `kprint!`/`kprintln!` (`core::fmt::Write`) |
| `klib/mmio.rs` | volatile MMIO accessors with a global VA offset (TTBR1 switch) |
| `klib/sync.rs` | `SpinLock<T>` and `SyncUnsafeCell<T>` |
| `klib/fmtbuf.rs` | `FmtBuf` — `core::fmt::Write` into a fixed `&mut [u8]` (the `ksnprintf` replacement) |
| `mm/consts.rs` | shared PTE format, VA layout, TTBR/ASID packing, `phys_to_virt`/`virt_to_phys` |
| `mm/pmm.rs` | bitmap physical page allocator (8 GiB), single + contiguous alloc |
| `mm/mmu.rs` | 4-level page tables, MMU enable, per-task user mappings, self-tests |
| `mm/heap.rs` | first-fit kernel heap (`kmalloc`/`kfree`) + `GlobalAlloc` adapter |
| `exception/mod.rs` | `TrapFrame`, ESR/DFSC decode, `exception_dispatch`, user-fault diagnostics |
| `exception/vector.S` | ARMv8 vector table + `exception_common` save/restore |
| `exception/gic.rs` | GICv3 distributor/redistributor bring-up, IRQ ack/EOI, per-INTID counters |
| `exception/timer.rs` | ARM generic timer (periodic tick → scheduler) |
| `sched/mod.rs` | preemptive round-robin scheduler, EL0/EL1 tasks, fork, kill, reap, ASIDs |
| `sched/switch.S` | `context_switch`, task trampolines, `fork_return` |
| `sched/elf.rs` | ELF64 ET_EXEC loader (PT_LOAD mapping, W^X, icache sync) |
| `syscall/mod.rs` | SVC dispatch (`x8`=number), user-pointer validation |
| `syscall/exec.rs` | `SYS_EXEC` — load ELF from VFS, build argv, swap TTBR0, rewrite frame |
| `drivers/pci/` | PCIe ECAM enumeration + BAR assignment |
| `drivers/virtio/` | VirtIO PCI transport + split virtqueue; rng/blk/net/console/balloon |
| `fs/vfs.rs` | vnode tree, path resolution, fd tables, `FileOperations` vtables |
| `fs/devices.rs` | `/dev/{console,null,zero,rng,vcons,blk}` |
| `fs/fat32.rs` | FAT32 read + write, VFS-backed via lazy directory lookup |
| `fs/proc.rs` | `/proc/{uptime,meminfo,tasks,interrupts,netinfo,cmdline,version,balloon,cpuinfo}` |
| `user/mod.rs` | EL0 programs that live in the kernel image: the shell, demo/crash/noop tasks |

---

## Boot flow

```
QEMU loads the ELF at PA 0x4000_0000 and enters _start (EL1, MMU off)
  │
arch/boot.S:_start
  ├─ park secondary CPUs (MPIDR Aff0 != 0)
  ├─ SP := __stack_top − KERNEL_VA_OFFSET   (physical stack)
  ├─ zero .bss (physical addresses)
  ├─ bl early_init                          (PC-relative → physical)
  │     main.rs::early_init  (runs physically, MMU off)
  │       ├─ cpu::enable_fp_simd            (core::fmt uses SIMD)
  │       ├─ uart::init
  │       ├─ pmm::init(0x4000_0000, 8 GiB)  (logging via aligned UART helpers)
  │       ├─ mmu::init                       → builds TTBR0 (identity low) +
  │       │                                    TTBR1 (high) tables, enables MMU
  │       ├─ cpu::print_current_el           (now safe: absolute-VA data maps)
  │       ├─ exception::init                 (VBAR_EL1)
  │       └─ mmu::run_tests                  (4 self-tests)
  ├─ SP := __stack_top                       (upper-half VA)
  └─ br kmain                                (absolute → upper half)
        main.rs::kmain  (runs in the higher half)
          ├─ mmio::switch_to_upper           (route MMIO via TTBR1)
          ├─ exception::init_upper           (relocate VBAR)
          ├─ pmm::relocate_upper             (bitmap → upper-half VA)
          ├─ heap::init + self-tests
          ├─ BRK self-test
          ├─ gic::init
          ├─ pci::enumerate_bus
          ├─ virtio rng / blk / net / console / balloon init
          ├─ cpu::cpu_init                   (PMU)
          ├─ vfs::init + devices::register + proc::init
          ├─ fat32::mount → /mnt/fat32
          ├─ sched::init + create task_shell / task_crash / netd / churn
          ├─ timer::init + start             (timer IRQs now drive preemption)
          └─ idle loop: reap() + wfi
```

Two boot-addressing hazards are handled deliberately (see `early_init`'s
comments): pre-MMU, RAM is Device memory (so `core::fmt`'s unaligned accesses
fault — early logging uses aligned UART helpers), and Rust constructs that
materialize *absolute* upper-half VA pointers (e.g. `match`→`&str` tables) fault
until the MMU maps the high half — so `print_current_el` runs *after* `mmu::init`.

---

## Memory layout

Physical (QEMU `virt`, 8 GiB RAM):

```
0x0000_0000  ── (low MMIO: GIC @ 0x0800_0000, UART @ 0x0900_0000)
0x1000_0000  ── PCI 32-bit MMIO window
0x4000_0000  ── RAM base (KERNEL_PA): kernel image, then PMM bitmap, then free
0x40_1000_0000 ── PCI ECAM
0x80_0000_0000 ── PCI 64-bit MMIO window (BARs assigned here)
0x2_4000_0000 ── RAM end (8 GiB)
```

Virtual:

```
0x0000_0000_0040_0000  USER_TEXT_BASE   ┐ per-task EL0 (TTBR0), ASID-tagged,
0x0000_0000_0080_0000  USER_STACK_TOP   ┘ nG=1; stack grows down, demand-paged
                                          to 256 KiB
0xFFFF_0000_0000_0000  KERNEL_VA_OFFSET ┐ kernel half (TTBR1), global mappings;
0xFFFF_0000_4000_0000  kernel image      ┘ phys_to_virt(pa) = pa + offset
```

`KERNEL_VA_OFFSET = 0xFFFF_0000_0000_0000`. Page tables are 4 KiB granule,
48-bit VA, 2 MiB L2 blocks for the kernel identity map; user maps use 4 KiB L3
pages. The kernel is linked at the high VA but loaded at the low PA via the
linker `AT()` override.

---

## Frozen ABI contracts

These are shared between Rust and the `.S` files; changing one without the
others is silent memory corruption. Compile-time `offset_of!` asserts guard the
struct layouts.

### Trap frame (`vector.S` ⇄ `exception::TrapFrame`)

688 bytes, 16-byte aligned. Byte offsets:

| Offset | Field |
|--------|-------|
| 0   | x0..x30 (`regs[31]`) |
| 248 | ELR_EL1 |
| 256 | SPSR_EL1 |
| 264 | ESR_EL1 |
| 272 | FAR_EL1 |
| 280 | **SP_EL0** (EL0 entry only; *not* a `TrapFrame` field — raw offset) |
| 288 | q0..q7, q16..q31 (caller-saved SIMD) |
| 672 | FPSR / 680 FPCR |

`SP_EL0_OFFSET = 280` is used by `sys_exec` to rewrite the user SP. q8–q15 are
callee-saved and preserved by `context_switch`, not the trap frame.

### Context-switch frame (`switch.S` ⇄ `sched`)

160 bytes: x19–x30 (96) + d8–d15 (64). `Task.sp` at offset 0 (`TASK_SP`),
`Task.ttbr0` at offset 40 (`TASK_TTBR0`) — both asserted via `offset_of!`.
`fork_return` restores a 288-byte slice (GPRs + ELR + SPSR + SP_EL0).

### Syscall ABI (`user` ⇄ `exception` ⇄ `syscall`)

`SVC #0`, `x8` = number, `x0..x7` = args, return in `x0`. Numbers:

```
READ=0 WRITE=1 OPEN=2 CLOSE=3 EXIT=4 YIELD=5 SLEEP=6 GETPID=7 LSEEK=8
UPTIME=9 NET_PING=10 KILL=11 FORK=12 EXEC=13 BALLOON=14 REBOOT=15
```

(`REBOOT=15` extends the original C ABI — see the reboot commit.) Dispatch
unmasks IRQs early so blocking syscalls are preemptible; every user buffer is
range-checked (`user_buf_ok`/`user_str_ok`, `checked_add` for overflow).

### Page-table entry format (`mm::consts`)

`PTE_VALID|TABLE`, AP bits (RW/RO × EL0), `PTE_AF`, `PTE_SH_INNER`, `PTE_NG`
(user maps; ASID-tagged), `PTE_UXN`/`PTE_PXN`, attr index (0=Device, 1=Normal).
ASID lives in `TTBR0[63:48]` (`TTBR_ASID_SHIFT=48`). User PTEs always set nG;
kernel PTEs leave it clear (global).

---

## Concurrency model

Single core. Shared mutable state is guarded one of three ways:

- **`SpinLock<T>`** — structured state mutated post-boot (PMM, heap, PCI, each
  VirtIO device, FAT32 volume).
- **`SyncUnsafeCell<T>`** — scheduler run queue + counters and the VFS vnode
  pool: mutated only with IRQs masked (`irq_save`/`irq_restore`) or during
  single-threaded boot. Every access is `unsafe` with a `// SAFETY (single-core)`
  note naming the invariant.
- **`addr_of!`/`addr_of_mut!` statics** — VirtIO DMA ring buffers (the sound,
  recommended pattern for DMA-visible memory that never forms a reference).

The scheduler run queue is intentionally lock-free (mutated only IRQ-masked) so
the timer-IRQ `schedule()` path can't deadlock on a lock.

---

## Test matrix

`ci/smoke-test.sh` (run on every push by `.github/workflows/ci.yml`, after
`clippy -D warnings` + debug/release builds) boots the kernel headless under
QEMU and asserts:

| Area | Assertion |
|------|-----------|
| MMU | `MMU Enabled` + `TTBR1 Upper Half` self-tests PASS |
| Heap | `coalesce + realloc` self-test PASS |
| Exceptions | `Survived BRK` (vector save/restore round-trips) |
| VirtIO RNG | `got 16 bytes` (DMA entropy) |
| VirtIO BLK | sector `write+read round-trip: PASS` |
| Networking | `DHCP Lease ACK` + `PING reply from 10.0.2.2` |
| FAT32 read | `/mnt/fat32/HELLO.TXT` contents |
| FAT32 write | `create+read RUSTW.TXT round-trip: PASS` |
| Scheduler | `CHURN TEST PASS` (48 task create/exit/reap, zero page leak) |
| Liveness | `KERNEL Ready!`, and no `KERNEL PANIC` / `RUST PANIC` / `FAIL` |
| EL0 shell | drives `pid` / `fork` / `balloon inflate` / `hexdump` / `exec HELLO.ELF` over stdin |

Built-in self-tests (`mmu::run_tests`, `heap::run_tests`, the BRK probe, and the
per-driver smoke reads) run on every boot regardless of CI.
