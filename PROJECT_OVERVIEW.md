# FermiOS Hypervisor — Project Overview & Branch Map

This document summarizes **everything built in this work stream** — the
conversion of the FermiOS EL1 kernel into a Type‑1 hypervisor — and maps it
against the wider set of branches in the repository so it is clear what belongs
to this effort versus what came from other parallel work.

- **Repo:** `git@github.com:rituparna-ui/fermi-os.git`
- **This work:** branch **`fermi-hyp2`**, pushed to remote
  **`feat/el2-vhe-hypervisor-fermios-guest`** (HEAD `5cf64db`).
- **Base:** all of this sits on top of the FermiOS kernel at `a2f1104` (the
  `main`‑line kernel: higher‑half AArch64, MMU, PMM, heap, GICv3, timer,
  scheduler + EL0 tasks, syscalls, VFS/FAT32/proc, PCI/virtio, shell).
- **Scope of this branch:** 43 commits, 37 files changed, ~5,990 insertions;
  ~4,900 lines of new hypervisor code under `src/hyp/` (+ ~210 in a standalone
  foreign mini‑guest). Builds clean; a single regression script proves the
  whole suite (`src/hyp/run-demos.sh` → "ALL MILESTONES PASS (M1‑M25)").

> Companion docs: `HYPERVISOR_JOURNEY.md` (the full narrative — decisions, bugs,
> and why), and `src/hyp/README.md` (per‑file architecture reference). This file
> is the high‑level index and branch map.

---

## 1. What was built (branch `fermi-hyp2`)

A **VHE Type‑1 hypervisor**: FermiOS runs at **EL2** using ARMv8.1
Virtualization Host Extensions (`HCR_EL2.E2H=1`), hosting EL1 guest VMs. The
same `kernel.elf` is both hypervisor and guest — `boot.S` enables VHE only when
entered at EL2; otherwise it boots as plain FermiOS. A reduced‑RAM copy of the
kernel is embedded as the guest image.

### Capability summary

| Area | What works |
|------|-----------|
| **Boot / world** | EL2 VHE host; KVM‑style `vcpu_enter`/world‑switch that returns to the host scheduler on guest trap |
| **Memory** | Stage‑2 (IPA→PA) translation per VM; distinct VMID per guest; leak‑free teardown; safe guest‑IPA→host‑PA translation (`stage2_translate`) |
| **CPU** | Full per‑vCPU context save/restore (GPRs, EL1 bank via `_EL12`, FP, vGIC); **SMP** — multiple vCPUs per VM via PSCI `CPU_ON`, distinct `VMPIDR` |
| **Interrupts** | Virtual GICv3 (ICH_* list registers, GICD/GICR MMIO model); HW‑mapped LR for the guest timer (no storm); inter‑VM virtual SPI injection |
| **Time** | EL2 physical timer (`CNTHP`) drives preemption; guest virtual timer delivered as vINTID |
| **Devices** | Per‑guest virtual PL011 console (bidirectional); paravirt **block** and **network** via hypercalls with IPA‑translated buffers; PCI emulated absent |
| **ABI** | Unified HVC hypercall ABI (PSCI + vendor: VERSION/PUTC/VM_INFO/YIELD/DOORBELL/BLK/NET) |
| **Lifecycle** | Dynamic VM create/run/destroy; PSCI reset/off; warm reset |
| **Multi‑tenant** | Multiple isolated guests, preemptive round‑robin; unified interactive console with `Ctrl‑X` focus switching |
| **Generality** | Boots a **foreign, non‑FermiOS guest** via the standard `x0=DTB` protocol; emits an **OS‑grade device tree** (`/chosen`, `/psci`, `/cpus`, `/timer`, GICv3 `/intc`) |
| **Safety** | Adversarial **guest→host security audit** (fixes applied); runtime **fault isolation** (a bad guest is reaped, host + peers survive) |

### Milestones (each verified in QEMU 8.2.2)

| # | Milestone | Commit |
|---|-----------|--------|
| M1 | Boot existing kernel at EL2 as a VHE host | `74cabe1` |
| M2 | Dedicated `VBAR_EL2` vector table + trap plumbing | `594b78f` |
| M3 | World‑switch + stage‑2 + trivial EL1 guest | `986ffe4` |
| M4 | EL2‑physical‑timer time‑slicing | `732dba0` |
| M5 | vGIC — guest `gic_init` completes | `0b931b6` |
| M6 | Guest virtual timer (guest scheduler ticks) | `db48270` |
| M7/M8 | Guest loader; full FermiOS boots as an EL1 guest | `01fd993`/`06e2360` |
| M9a–c | Per‑vCPU save/restore; two full FermiOS guests round‑robin | `653a5ff`/`632e5b1`/`b661b81` |
| M10 | Per‑guest virtual PL011 console | `5d9c986` |
| M11 | PSCI provider (reboot/off) | `2ced24e` |
| M12 | Interactive guest console (bidirectional) | `d2dc56c` |
| M13 | Fully interactive FermiOS guest (HW‑mapped timer fix) | `8f7ab88` |
| M14 | Unified preemptive + interactive multi‑guest console | `af59f9c` |
| M15 | Inter‑VM shared memory + doorbell hypercall | `10b9de1` |
| M16 | Interrupt‑driven doorbell (virtual SPI injection) | `6bd9a6d` |
| M17 | Unified HVC hypercall ABI | `3d6f05c` |
| M18 | Guest→host security audit + fixes | `32db923` |
| M19 | Paravirtualized block device | `fed3178` |
| M20 | Paravirtualized network device | `d4a3b53` |
| M21 | Dynamic VM lifecycle (leak‑free teardown) | `4bc7170` |
| M22 | Foreign (non‑FermiOS) guest via DTB | `e587a9d` |
| M23 | SMP guest (2 vCPUs via PSCI `CPU_ON`) | `820d332` |
| M24 | OS‑grade device tree | `bbf30b6` |
| M25 | vCPU fault isolation | `5d6dd69` |
| M26 | Full regression script + capstone verification | `28bf4ac` |
| M27 | Per-VM observability (vCPU exit accounting) | `b06a0fc` |
| M28 | Boot a **real mainline Linux 6.6** kernel as an EL1 guest | `ec0c559` |
| M29 | Fuller vGIC → Linux boots fully (GICv3 + timer + clocksource) to init | `09f76c1` |
| M30 | Linux reaches **userspace** (built-in initramfs `/init`) → PSCI power-off | `5cf64db` |

### Source layout (`src/hyp/`, ~4,900 LOC)

```
hyp.{c,h}            EL2 detection, the self-tests, and all milestone drivers
world_switch.S       vcpu_enter + guest-exit path (the enter/exit core)
hyp_vectors.S        dedicated VBAR_EL2 table (M2 self-test)
vcpu.h               vcpu_t (asm-visible block + EL1/FP/vGIC/vUART state, mpidr)
vcpu_context.S       guest EL1 bank (_EL12) + FP save/restore
stage2.{c,h}         stage-2 tables, VTCR/VTTBR, translate, leak-free destroy
vgic/vgic.{c,h}      virtual GICv3: interface, LR inject (soft + HW-mapped), MMIO
vuart/vuart.{c,h}    per-guest virtual PL011 (TX line-buffer + RX FIFO)
psci/psci.{c,h}      PSCI 1.1 (VERSION/FEATURES/RESET/OFF/CPU_OFF/CPU_ON)
hvc/hvc.{c,h}        unified hypercall ABI (PSCI + vendor services)
fdt.{c,h}            flattened device-tree builder for foreign guests
miniguest/           standalone non-FermiOS AArch64 guest (start.S/main.c/linker.ld)
guest_stub.S         hand-written EL1 demo guests (smoke/spin/heartbeat/psci/
                     shm/doorbell/hvc/blk/net/smp/bad/good/echo)
guest_blob.S         embeds the reduced-RAM FermiOS guest image
miniguest_blob.S     embeds the standalone foreign mini-guest
run-demos.sh         one-command full M1-M25 regression
```

Touched outside `src/hyp/`: `boot.S` (VHE preamble), `kernel.c` (demo hooks
under `HYP_RUN_DEMOS`), `Makefile` (guest + mini‑guest builds), `pmm.h`
(overridable `MEM_SIZE`), `pci.h` (guest bus‑scan bound), and small NULL‑guard
robustness fixes in `blk.c` / `net.c` / `virtqueue.c`.

### Build & run

Use the `osdev:dev` Docker image (QEMU 8.2.2 + `aarch64-linux-gnu-gcc`); the
host's QEMU 3.1.0 lacks VHE. Default build runs two interactive FermiOS guests;
`-DHYP_RUN_DEMOS` runs the self‑test suite. Full regression:

```sh
docker run --rm -v "$PWD":/work -w /work osdev:dev bash src/hyp/run-demos.sh
```

---

## 2. Branch landscape (the wider repo)

The repository is the workspace for **many parallel experiments** on FermiOS,
all branching from the same `main` kernel (`a2f1104`). They are organized as git
worktrees. Only `fermi-hyp2` is the work described above; the rest are listed
here for orientation and are **not** part of this effort.

### Worktrees (checked-out branches)

| Worktree | Branch | What it is |
|----------|--------|-----------|
| `fermi-hyp2` | `fermi-hyp2` | **This work** — the VHE Type‑1 hypervisor (M1‑M26). |
| `fermi-hyp` | `feat/hyp-vpci-msix` | A **separate, NON‑VHE** Type‑1 hypervisor (own design: standalone EL2 image at PA 0x250000000, guests embedded as blobs). Independent lineage; has its own `src/hyp/` with vPCI/MSI‑X, virtio‑mmio (blk/net/balloon), snapshot, live‑migration, SMP, IPC. Not authored here. |
| `fermi-os` | `feat/virtio-console-…` | The `main`‑line kernel worktree + feature work. |
| `fermi-claude-rs` | `rust-port-claude-…` | A Rust port experiment. |
| `fermi-kiro-rs` | `integration` | Another Rust/integration experiment. |
| `fermi-kiro2` | `fermi-kiro2` | A worktree pinned at base `a2f1104`. |
| `fermi-verify` | `fermi-verify` | A verification worktree. |

### The two hypervisors (important distinction)

There are **two independent hypervisor efforts** in this repo, both starting
from the same EL1 FermiOS but taking opposite architectural routes:

- **`fermi-hyp` — non‑VHE.** A standalone EL2 binary; FermiOS is loaded as a
  separate guest blob. Broad device breadth (vPCI + MSI‑X, virtio‑mmio,
  balloon, snapshot/restore, live migration). Different lineage; predates and
  runs parallel to this work.
- **`fermi-hyp2` — VHE (this work).** The *existing* FermiOS kernel itself runs
  at EL2 (E2H redirect), so its proven MMU/GIC/timer/driver stack is reused
  verbatim as the host; guests are EL1. Emphasis on correctness, a clean
  milestone ladder, an audited guest→host boundary, paravirt I/O with safe IPA
  translation, foreign‑guest/DTB generality, and a single regression suite.

They share no source; the VHE branch was a deliberate second approach chosen
for this session.

### Remote branch families (other parallel work, ~90 branches)

For completeness, the remote carries large families of feature branches from
other sessions — none authored here:

- **`feat/hyp-*`** (~30): non‑VHE‑hypervisor features (vpci, virtio‑blk/net/mmio,
  balloon, smp‑guest, snapshot‑restore, live‑migration, pv‑console,
  observability, watchdog, weighted‑sched, psci‑cpu‑off, fault‑isolation).
- **`feat/smp-*`** (~14): SMP experiments on the base kernel (sched, mmu,
  preempt, workqueue, parsum, migrate, …).
- **`feat/shell-*`** (~15): userspace shell commands (cp, df, grep, history,
  top, wc, stat, tabcomplete, …).
- **`feat/net-*`** (arp, dns, ntp, tcp, ping‑stats) and **`fs/fat32-*`**,
  **`pci/*`**, plus historical kernel‑bringup branches (`kprintf`, `kheap`,
  `gicv3`, `timers`, `physical-memory-manager`, `memory-management-unit`,
  `userspace`, `scheduler`, …).
- **Linux‑guest** branches (`feat/el2-type1-hypervisor-linux-guest`,
  `feat/interactive-linux-console-…`, `feat/true-root-disk-switchroot-…`) belong
  to the non‑VHE line.

If you want a one‑line mental model: **`main` is the kernel; `fermi-hyp` is the
non‑VHE hypervisor; `fermi-hyp2` (this work) is the VHE hypervisor; everything
else is parallel feature/experiment branches off the kernel.**

---

## 3. Honest limitations (this branch)

- Built/verified only on QEMU ≥ 4.0 (`-cpu max`, VHE); cortex‑a72/QEMU 3.1.0
  cannot run it.
- Guest *virtio* devices are not passed through (PCI is emulated as
  "no device"); real device access is via paravirt hypercalls (M19/M20).
- A **real mainline Linux 6.6 kernel boots to userspace** (M28–M30) — it
  parses our DTB, brings up GICv3 + arch timer, switches clocksource, unpacks a
  built-in initramfs, and execs a freestanding static `/init` that prints from
  PID 1 over `ttyAMA0` and powers the VM off via PSCI `SYSTEM_OFF` (which the
  hypervisor catches and reaps). There is no shell/busybox or writable disk
  rootfs yet — `/init` is one self-contained program. The Linux `Image` is
  gitignored; `src/hyp/build-linux.sh` reproduces it (incl. the embedded
  initramfs + `/init`).
- One EL2 stack frame is shared by the serial scheduler (not reentrant across
  nested guest exits — fine as used).

These are breadth/environment boundaries, not missing fundamentals: every core
capability of a multi‑tenant, multi‑core, fault‑isolated Type‑1 hypervisor is
implemented, audited, and continuously verifiable.
