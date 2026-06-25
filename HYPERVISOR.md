# Fermi Hypervisor (EL2) — Design Notes

This document describes the Type-1 (bare-metal) hypervisor layer that Fermi OS
grows into when launched at **EL2**. It boots first at EL2, sets up second-stage
translation, and runs several mutually-isolated guests under preemptive
scheduling:

- **vCPU 0** — Fermi itself, running unmodified at EL1.
- **vCPU 1** — an unmodified aarch64 **Linux** kernel, booted to a BusyBox
  userspace shell (its console is captured; see §10).
- **vCPU 2** — a tiny silent bare-metal payload demonstrating that the vCPU
  table / scheduler / per-guest stage-2 generalise past two guests. Its
  progress is visible via `/proc/vms`.

The vCPU count is just `NUM_VCPUS`; the round-robin scheduler and per-guest
stage-2 (one VMID each) scale to N.

It targets QEMU's `virt` machine with `gic-version=3,virtualization=on` and a
Cortex-A72 (ARMv8.0-A, **no VHE**). The code lives in `src/hyp/`.

---

## 1. Exception-level model

QEMU enters the image at the highest implemented EL. With `virtualization=on`
that is **EL2**. `boot.S` reads `CurrentEL`:

- **EL2** → call `hyp_init()` (configure the hypervisor), then `eret` to EL1 to
  run the primary guest. The EL1 continuation is the *original* Fermi boot path,
  so Fermi runs unchanged as a guest.
- **EL1** (machine started without `virtualization=on`) → skip the hypervisor
  entirely and boot as a plain kernel. The EL2 layer is fully optional.

Because Cortex-A72 has no VHE, the hypervisor runs at bare EL2 with its **MMU
off** — every pointer it uses is physical. (The kernel is linked at a high-half
VA but all symbol references are PC-relative under `-fno-pic`, so taking a
symbol's address pre-MMU yields its physical address. This is the same trick the
pre-MMU `early_init()` relies on.)

A dedicated EL2 vector table (`VBAR_EL2`, `src/hyp/vector_el2.S`) and a private
EL2 stack handle every trap from the guests.

---

## 2. Second-stage (stage-2) translation & the physical memory map

Stage-2 translates each guest's Intermediate Physical Addresses (IPA) to real
Physical Addresses (PA), per guest, via `VTTBR_EL2`/`VTCR_EL2`. Each guest has a
distinct **VMID**, so swapping `VTTBR_EL2` on a world switch needs no TLB flush.

QEMU is given **10 GiB** of RAM (`-m 10G`), laid out as:

```
phys 0x40000000 .. 0x240000000   (1..9 GiB)   Fermi's RAM (PMM manages 8 GiB)
  └─ within it, .hyp region                   hypervisor-private (tables, EL2
                                              stack, vCPU blocks, vGIC state)
phys 0x240000000 .. 0x280000000  (9..10 GiB)  Linux guest's RAM (1 GiB)
```

- **Fermi (vCPU 0)** gets a 1 TiB identity stage-2 (1 GiB blocks): RAM as Normal,
  everything else (GIC, UART, PCI) as Device. Two regions are then carved out:
  - The **`.hyp` region** is split to 4 KiB granularity and **unmapped**, so a
    stray guest access to hypervisor memory faults to EL2 (see §8).
  - The **Linux guest's 1 GiB window** is unmapped (its whole 1 GiB stage-2
    block is cleared), so Fermi cannot see or corrupt Linux's memory.
- **Linux (vCPU 1)** gets a tiny stage-2 mapping **only** its own 1 GiB window
  (guest IPA `0x40000000` → phys `0x240000000`, Normal). Every other IPA is
  unmapped, sandboxing it. The GIC region **and the PL011 UART** are left
  unmapped so that the guest's GIC MMIO and console writes trap to EL2 for
  emulation (§6, §7). Linux's console output is captured into a hypervisor
  buffer rather than written to the shared serial (§10).

The result is **bidirectional isolation**: neither guest can reach the other's
RAM, and neither can reach the hypervisor's.

---

## 3. vCPU model & world switch

A `vcpu_t` (in hypervisor-private `.hyp` memory) holds everything needed to
suspend and resume a guest:

- **GP registers** `x0..x30` (saved from / restored to the EL2 trap frame).
- **PC / PSTATE** = `ELR_EL2` / `SPSR_EL2`.
- **Per-guest `VTTBR_EL2`** (stage-2 base | VMID).
- **EL1 system-register bank**: `SP_EL1`, **`SP_EL0`**, `ELR_EL1`, `SPSR_EL1`,
  `SCTLR_EL1`, `CPACR_EL1`, `TTBR0/1_EL1`, `TCR_EL1`, `MAIR/AMAIR_EL1`,
  `VBAR_EL1`, `CONTEXTIDR_EL1`, `TPIDR_EL1`, `TPIDRRO_EL0`, `TPIDR_EL0`,
  `ESR/FAR/PAR_EL1`.
- **vGIC state**: list registers `ICH_LR0/1_EL2`, `ICH_VMCR_EL2`,
  `ICH_AP1R0_EL2`.
- **FP/SIMD**: `q0..q31`, `FPSR`, `FPCR`.

`hyp_world_switch()` saves the outgoing vCPU (FP first, while it is still live
and before any C in the path touches SIMD), then restores the next one (FP last,
so nothing clobbers it before `eret`). The physical timer is **not**
context-switched — see §6.

> **The `SP_EL0` bug.** arm64 Linux keeps the `current` task pointer in
> `SP_EL0`. The world switch originally saved `SP_EL1` but not `SP_EL0`. Fermi
> tolerated this because it rewrites `SP_EL0` on every EL0 entry, but Linux
> relies on it persisting across kernel execution. Symptom: after a few quanta
> Linux took a recurring EL1 data abort reading `~0x7fffXX` — exactly Fermi's
> EL0 user stack (`USER_STACK_TOP = 0x800000`), which had leaked into the Linux
> vCPU. Linux then dereferenced `current` through Fermi's stale stack pointer
> and stormed on the fault. Adding `SP_EL0` to the saved context fixed it and
> was the single change that took Linux from "hangs in mm init" to "boots to
> userspace."

---

## 4. Scheduling

The hypervisor owns the **EL2 physical timer** `CNTHP_EL2` (PPI / INTID 26),
armed for a 100 ms quantum. On each tick the hypervisor world-switches between
runnable vCPUs round-robin — no guest cooperation required. The scheduler
generalizes to N vCPUs; the picker simply skips `UNUSED` ones.

A guest can also yield cooperatively via `HVC_YIELD`, and can remove itself via
PSCI `SYSTEM_OFF` (§7), after which it is never scheduled again.

---

## 5. Boot flow summary

```
QEMU (-kernel) ─▶ EL2: boot.S ─▶ hyp_init():
    • EL2 MMU off, EL2 vector table (VBAR_EL2), private stack
    • stage-2 for vCPU0 (identity) + carve out .hyp and Linux window
    • CNTVOFF_EL2 = 0, CNTHCTL_EL2 allows EL1 timer access
    • EL2 physical GIC CPU interface + virtual CPU interface (ICH_HCR_EL2)
    • CPTR_EL2.TFP = 0 (EL2 may run FP for context switch)
    • create vCPU1 (Linux): its stage-2, entry = Image, x0 = DTB
    • arm CNTHP scheduling tick
    • HCR_EL2 = RW | VM | IMO
  ─▶ eret to EL1 ─▶ Fermi boots as vCPU0; CNTHP later preempts in Linux
```

---

## 6. Interrupt & timer virtualization (vGIC)

Physical IRQs are routed to EL2 (`HCR_EL2.IMO`). The hypervisor owns the
physical GICv3 CPU interface; the guests' `ICC_*` accesses are transparently
redirected by hardware to the **virtual** CPU interface (no emulation needed for
the CPU interface). The distributor/redistributor *is* emulated for Linux (§7).

On a physical IRQ taken at EL2:

- **INTID 26 (`CNTHP`)** — the hypervisor's own scheduling tick: re-arm, fully
  EOI + deactivate (`ICC_DIR`), then world-switch.
- **Any other INTID** — ack it, then inject a **hardware-linked virtual
  interrupt** into the owning vCPU's list register (`ICH_LR<n>.HW=1`, mapping
  vINTID→pINTID), and priority-drop. The guest's own EOI on the virtual
  interface then deactivates the physical interrupt. If the owner is not
  currently running, the vIRQ is stashed in its *saved* list-register state and
  delivered when it is next scheduled (ownership routing).

**Three independent timers, no conflict:**

| Timer | INTID | Owner |
|-------|-------|-------|
| `CNTP` (EL1 physical) | 30 | Fermi (vCPU 0) |
| `CNTV` (virtual)      | 27 | Linux (vCPU 1) |
| `CNTHP` (EL2)         | 26 | hypervisor scheduler |

Because they are distinct hardware timers, each guest's timer state lives in its
own registers and needs no context switch.

---

## 7. Trap-and-emulate

- **Hypercall ABI** (`src/hyp/hypercall.h`): SMCCC-style `HVC` with the function
  ID in `x0`, args in `x1`–`x3`, result in `x0`. Calls: `VERSION`, `PUTC`
  (paravirt console), `PING`, `YIELD`, `VM_INFO`, `HYP_BASE`, and introspection
  (`VM_COUNT`, `VM_STAT`).
- **PSCI** (over `HVC`, function-ID space `0x8400_00xx`): `PSCI_VERSION`,
  `SYSTEM_OFF`/`SYSTEM_RESET` (reap the calling vCPU). Linux's DT declares
  `psci { method = "hvc"; }`, so its PSCI calls land here.
- **System registers** (EC `0x18`): infrastructure to decode `ESR_EL2` ISS and
  emulate `MRS/MRS`, stepping `ELR_EL2` past the trapped instruction. (The M2
  `HCR_EL2.TID3` ID-trap demo is left disabled by default — guests read ID
  registers natively.)
- **Emulated GICv3 distributor/redistributor**: the Linux guest's GIC MMIO
  region is unmapped in stage-2, so accesses trap to EL2 as data aborts. A
  minimal software model answers the reads Linux's driver validates
  (`PIDR2` ⇒ v3, `TYPER`, redistributor `WAKER` handshake / `Last` bit) and
  accepts the configuration writes. The data-abort ISS gives access size,
  direction, and register, so the load/store is emulated and `ELR_EL2` stepped.
- **Emulated PL011 UART**: the Linux guest's UART is likewise unmapped and
  emulated. DR writes are captured into a hypervisor ring buffer; the flag
  register reports "TX ready, RX empty" and the PrimeCell/peripheral ID
  registers are emulated so Linux's amba bus binds the pl011 driver (ttyAMA0).
  The console is output-only by design (RX always empty); see §10.

---

## 8. Isolation enforcement

- The `.hyp` region is unmapped from every guest's stage-2. A guest read/write
  there faults to EL2; the handler reports it, delivers a poison value (0) on a
  read, and steps over the access so the guest continues — proving the boundary
  without crashing the guest.
- The Linux guest's RAM window is unmapped from Fermi's stage-2 (and Linux's
  stage-2 maps only its own window), giving full bidirectional isolation.
- An *unexpected* abort from the Linux guest reaps that vCPU (keeping Fermi and
  the hypervisor alive); an unexpected abort from the primary guest parks the
  CPU (it indicates a real bug).

---

## 9. Linux guest bring-up

- **Device tree** (`guest.dts` → `build/guest.dtb` via `dtc`): 1 GiB memory at
  IPA `0x40000000`, PL011 earlycon, `psci { method = "hvc"; }`, the armv8 timer,
  a GICv3 node, and `chosen` with the bootargs + initrd range.
- **Loading**: QEMU's generic loader stages the kernel `Image`, the DTB, and the
  busybox initramfs into the Fermi-invisible high RAM (phys `0x240200000` /
  `0x248000000` / `0x24a000000`), which the Linux stage-2 maps to IPA
  `0x40200000` / `0x48000000` / `0x4a000000`.
- **Entry**: per the arm64 boot protocol — `PC = Image base`, `x0 = DTB`, EL1h,
  MMU off, `x1..x3 = 0`.
- **Result**: GICv3 driver initialises on the emulated distributor, the
  architected virtual timer runs (clock advances, delay loop calibrates), SMP
  bringup completes, the kernel unpacks the initramfs, runs `/init`, and drops
  to an interactive BusyBox shell — concurrently with Fermi.

> **SCS caveat.** Recent Ubuntu generic arm64 kernels enable
> `CONFIG_SHADOW_CALL_STACK`; their exception-handler prologue does
> `str x30, [x18]` against a shadow-stack pointer that is unmapped under this
> minimal boot, producing a nested-exception storm. Use an **SCS-free** kernel
> (e.g. Ubuntu 5.4 generic). `scripts/stage-linux-guest.sh` fetches one.

---

## 10. Introspection

`cat /proc/vms` (from the Fermi EL0 shell) renders a live vCPU table by
hypercalling from the EL1 `/proc` generator:

```
Fermi hypervisor (EL2): 2 vCPUs, 160 world-switches
VCPU NAME   STATE    HVC    SYSREG ABORT VIRQ   MMIO
0    Fermi  RUNNING 12 0 1 1539 0
1    Linux  READY   6 0 0 371 85
```

Fermi shows timer vIRQs and no MMIO (it drives the physical GIC directly); Linux
shows PSCI hypercalls, virtual-timer vIRQs, and emulated GIC MMIO accesses.

**`cat /proc/linux_console`** dumps the Linux guest's captured console output
(its PL011 is emulated, so its output goes to a hypervisor buffer instead of the
shared serial). This keeps Fermi's serial clean and interactive while still
exposing Linux's full boot log and shell output on demand.

---

## 11. Reproducing

```bash
./scripts/stage-linux-guest.sh   # on a host with internet: fetch Image + build initramfs
make run                         # inside the build container (see README)
```

`make run` boots Fermi-the-hypervisor; you get Fermi's shell and the Linux
BusyBox shell interleaved on the same serial console.

---

## 12. Known limitations / future work

- Single physical CPU; guests are pinned to one vCPU each (the DT advertises one
  CPU to Linux).
- The emulated GICv3 covers what Linux's boot path needs (PPIs via injection +
  distributor/redistributor identity reads); SPI routing for emulated devices is
  not modelled.
- The Linux guest's PL011 is emulated and its output is captured to a
  hypervisor buffer exposed as `/proc/linux_console` (output-only — RX reads
  empty, so the Linux shell cannot currently be typed into). This keeps the
  shared serial clean for Fermi; a truly separate *interactive* console would
  need a second UART, which QEMU `virt` does not provide.
- Requires an SCS-free guest kernel (see §9).
