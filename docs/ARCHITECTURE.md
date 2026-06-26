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
| `fs/proc.rs` | `/proc/{uptime,meminfo,tasks,interrupts,netinfo,cmdline,version,balloon,cpuinfo,vms}` |
| `user/mod.rs` | EL0 programs that live in the kernel image: the shell, demo/crash/noop tasks |
| `hyp/mod.rs` | EL2 Type-1 hypervisor: `hyp_init`, stage-2 build + isolation, world-switch, trap dispatch (HVC/sysreg/abort/IRQ), vGIC injection, emulated GICv3 MMIO, PSCI, Linux-slot guest |
| `hyp/hypercall.rs` | SMCCC-style HVC ABI shared by guest (EL1) and hypervisor (EL2): function IDs + `hvc_call` trampoline |
| `hyp/vector_el2.S` | EL2 exception vector table + `el2_common` save/restore/eret trampoline (256-byte x0..x30 frame) |
| `hyp/linux_stub.S` | M10 position-independent guest stand-in (writes `L` via the guest's stage-2 UART mapping); retained as a fallback |

---

## Boot flow

```
QEMU loads the ELF at PA 0x4000_0000 and enters _start
  (EL1 by default, or EL2 if launched with virtualization=on)
  │
arch/boot.S:_start
  ├─ x28 := 0                               (entry-EL indicator, callee-saved)
  ├─ park secondary CPUs (MPIDR Aff0 != 0)
  ├─ SP := __stack_top − KERNEL_VA_OFFSET   (physical stack, for the entry EL)
  ├─ if CurrentEL == EL2:                    (only with virtualization=on)
  │     ├─ x28 := 1
  │     ├─ bl hyp_init                       (configure EL2; see Hypervisor §)
  │     ├─ SP_EL2 := el2_stack top          (dedicated EL2 trap stack)
  │     └─ eret → el1_entry (EL1h)          (drop to the guest)
  ├─ el1_entry: SP := __stack_top − KERNEL_VA_OFFSET
  ├─ zero .bss (physical addresses)         (x28 survives — only x0-x2 touched)
  ├─ bl early_init(x28)                     (PC-relative → physical)
  │     main.rs::early_init  (runs physically, MMU off)
  │       ├─ hyp::set_booted_via_el2(x28)   (durable post-zero_bss)
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

### EL2 trap frame (`vector_el2.S` ⇄ `hyp::El2Frame`)

The EL2 vector stub pushes only the GP registers: `x[31]` = x0..x30 (248 bytes),
in a 256-byte stack reservation (16-byte alignment). PC/PSTATE come from
`ELR_EL2`/`SPSR_EL2`; everything else a guest needs (EL1 sysregs, vGIC, FP) is
saved into the `Vcpu` block by the world-switch, not the frame. Each of the 16
vector slots is ≤ 0x80 bytes (table 0x800-aligned); the stub records the slot
index and branches to `el2_common`, which saves x2–x30, calls `el2_dispatch`,
restores, and `eret`s. Slot kind = `index & 3` (0=sync, 1=IRQ).

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

Single core. Shared mutable state is guarded one of four ways:

- **`SpinLock<T>`** — structured state mutated post-boot but NOT touched by an
  IRQ handler (PMM, heap, PCI, each VirtIO device, FAT32 volume).
- **`SpinLockIrqSafe<T>`** — state locked from BOTH task context and an IRQ
  handler (the timer). `lock()` masks IRQs before acquiring and the guard
  restores them on drop, so a tick can't deadlock by spinning on a lock the
  preempted task holds.
- **`SyncUnsafeCell<T>`** — scheduler run queue + counters and the VFS vnode
  pool: mutated only with IRQs masked (`irq_save`/`irq_restore`) or during
  single-threaded boot. Every access is `unsafe` with a `// SAFETY (single-core)`
  note naming the invariant.
- **`addr_of!`/`addr_of_mut!` statics** — VirtIO DMA ring buffers (the sound,
  recommended pattern for DMA-visible memory that never forms a reference).

The scheduler run queue is intentionally lock-free (mutated only IRQ-masked) so
the timer-IRQ `schedule()` path can't deadlock on a lock.

The EL2 hypervisor's mutable state (`VCPUS[2]`, `CURRENT_VCPU`, switch/quantum
counters, the stage-2 tables, the emulated-GIC state) lives in `SyncUnsafeCell`
statics in the `.hyp_tables` section. It is touched only from EL2 trap context,
which cannot re-enter on a single core, so no lock is needed; each access is
`unsafe` with a `// SAFETY (single-core)` note.

---

## Hypervisor (EL2 Type-1 VMM)

Launched with QEMU `virtualization=on`, the image enters at **EL2**. `boot.S`
detects this, calls `hyp::hyp_init`, and `eret`s to EL1 where the rest of the
kernel runs unchanged as a **stage-2-translated guest** (vCPU 0). Entered at EL1
(no `virtualization=on`), the entire EL2 layer is skipped — fully backwards
compatible. `early_init` records the entry EL (`hyp::set_booted_via_el2`) so the
EL1 guest knows whether issuing an `HVC` is safe.

Everything in `hyp_init` runs at EL2 with the **EL2 MMU off**: all addresses are
physical (PC-relative), and it logs via a self-contained PL011 writer — never
`core::fmt`, which would materialize an unmapped upper-half VA. The hypervisor's
own RAM lives in a NOLOAD `.hyp` linker section placed *after* `__bss_end` (so
the guest's `zero_bss` can't wipe the live stage-2 tables) and *before*
`__kernel_end` (so the guest PMM reserves, not reuses, those pages).

**Stage-2 & isolation.** Per-guest IPA→PA tables via `VTTBR_EL2`/`VTCR_EL2`
(4 KiB granule, 48-bit IPA, 40-bit PA), distinct VMIDs so no TLB flush on a
VTTBR swap. vCPU 0 gets a 1 TiB identity map (1 GiB blocks); the one block
holding `[__hyp_start, __hyp_end)` is split to 4 KiB and the hyp pages left
invalid. A guest touch of hyp memory faults to EL2, is reported, the read is
poisoned to 0, and the instruction is stepped over.

**Trap dispatch** (`el2_dispatch`): slot kind 1 = IRQ → `hyp_handle_irq`; kind 0
= sync, decoded by `ESR_EL2.EC`: HVC (hypercall — ELR already past it), trapped
MSR/MRS (`HCR_EL2.TID3`, emulate + ELR+=4), or a lower-EL data/instruction abort
(isolation / emulated-GIC / reap, ELR+=4 where serviced).

**Virtual interrupts.** Physical IRQs route to EL2 (`HCR_EL2.IMO`); each is
acked, re-injected to its owning vCPU as a hardware-linked vIRQ via an
`ICH_LR<n>_EL2` list register (HW=1, so the guest's own EOI deactivates the
physical interrupt), then priority-dropped (`EOImode=1`). The hypervisor's own
scheduler tick (CNTHP, INTID 26) is the exception: it fully EOI+deactivates and
is never injected. Per-guest `ICH_LR`/`VMCR`/`AP1R0` are saved/restored across
switches; CNTV (PPI 27) routes to the Linux vCPU.

**Preemptive world-switch.** `CNTHP_EL2` fires a 100 ms quantum; each tick
round-robins the vCPUs. A switch saves the outgoing guest's GP regs (from the
EL2 frame), PC/PSTATE/VTTBR, the full EL1 sysreg set, vGIC state, and FP/SIMD
(q0–q31 + FPSR/FPCR) into its `Vcpu`, then loads the next — FP saved *first*
(guest FP still live) and restored *last* (nothing after touches SIMD).

**Hypercall ABI & lifecycle.** SMCCC-style `HVC` (fn in x0, args x1–x3, result
x0): VERSION / PUTC / PING / VM_INFO / YIELD / HYP_BASE / VM_COUNT / VM_STAT.
PSCI `SYSTEM_OFF`/`RESET` reaps the calling vCPU. `/proc/vms` cats live vCPU
state over VM_COUNT/VM_STAT.

**Linux slot (vCPU 1).** A 1 GiB RAM window at IPA `0x40000000` backed by
host-invisible RAM at 9 GiB; the GIC window is intentionally unmapped so guest
GICD/GICR MMIO traps to EL2 and is emulated (`hyp_emulate_gic`) enough for
Linux's gic-v3 probe. Entry follows the arm64 boot protocol (PC = Image base,
x0 = DTB). The Image/initramfs are external (uncommitted) assets; without one
the slot faults immediately and is reaped, leaving the primary guest running.

Stage-2 needs QEMU ≥ 8 (the host's 3.1.0 faults at stage-2 level 0 — an emulator
limitation). `ci/hyp-smoke-test.sh` runs it under QEMU 8.x.

---

## Test matrix

`ci/smoke-test.sh` (run on every push by `.github/workflows/ci.yml`, after
`cargo fmt --check` + `clippy -D warnings` + debug/release builds) boots the
kernel headless under QEMU and asserts:

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
| FAT32 subdir | `mkdir RDIR + create RDIR/INNER.TXT + read: PASS` |
| FAT32 rm | `create+exists+remove+gone+recreate: PASS` |
| FAT32 cycle | 5× create/verify/rm, FAT free-space stable (cluster reuse) |
| Sched churn | `CHURN TEST PASS` (48 task create/exit/reap, zero page leak) |
| Heap stress | `HEAP STRESS PASS` (>1 MiB expand + coalesce, zero byte leak) |
| FD stress | `FD STRESS PASS` (64-fd exhaustion, reuse, bad-fd) |
| Fork stress | `FORK STRESS` (16 forks, frame copy + reap) |
| ASID wrap | `ASID WRAP PASS` (65535→1 boundary, global TLB flush, risk R3) |
| Liveness | `KERNEL Ready!`, and no `KERNEL PANIC` / `RUST PANIC` / `FAIL` |
| EL0 shell | drives `pid` / `mkdir` / `rm` / `ls` / `fork` / `balloon inflate` / `hexdump` / `exec HELLO.ELF` over stdin |

Built-in self-tests (`mmu::run_tests`, `heap::run_tests`, the BRK probe, and the
per-driver smoke reads) run on every boot regardless of CI.

`ci/hyp-smoke-test.sh` (also CI-gated) boots the kernel **as a hypervisor**
(`virtualization=on`, entering at EL2, under QEMU ≥ 8) and additionally asserts:

| Area | Assertion |
|------|-----------|
| EL2 bring-up | `Fermi hypervisor online at EL2` + `stage-2 enabled … dropping to EL1 guest` |
| Isolation | `ISOLATION: blocked guest read from hyp memory` + `stage-2 isolation held` (poisoned to 0) |
| Virtual IRQ | `injected hw vIRQ intid=` (physical IRQ re-injected to a vCPU) |
| Second guest | `created Linux-slot guest (vCPU 1)` + `preemptive scheduler armed (CNTHP tick)` |
| Introspection | `/proc/vms` reports `2 vCPUs` with live per-vCPU stats |
| Lifecycle | Linux slot faults without an Image and is reaped; primary guest survives |
| Guest liveness | the EL1 guest still reaches `KERNEL Ready!` and passes its stress tests under stage-2 + preemption, with no EL2 trap |
