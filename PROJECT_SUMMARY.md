# Fermi OS → Type-1 Hypervisor: Project Summary & Branch Index

This document is a high-level index of everything built while turning **Fermi OS**
(a from-scratch aarch64 kernel) into a **Type-1 (bare-metal) EL2 hypervisor** that
runs a real, SMP **Linux** guest — and maps each capability to the git branch that
delivered it.

Companion documents:
- [`HYPERVISOR.md`](HYPERVISOR.md) — steady-state architecture/design reference.
- [`DEVLOG.md`](DEVLOG.md) — the narrative development log (the *why*, the bugs, the fixes).
- [`README.md`](README.md) — the kernel + hypervisor feature overview.

All work was developed and verified inside the `osdev` Docker container (aarch64
cross-toolchain + QEMU), and pushed to `git@github.com:rituparna-ui/fermi-os.git`
on **uniquely-named feature branches** (never to `main`, never via PR).

---

## 1. What the system does today

`make run` boots Fermi **at EL2 as a Type-1 hypervisor**, which then runs several
mutually-isolated guests under preemptive round-robin scheduling on a single
physical CPU:

- **vCPU 0 — Fermi OS** itself, unmodified, at EL1 (the primary guest).
- **vCPU 1 + vCPU 2 — a 2-core SMP Linux** (`nproc` = 2) that:
  - boots an unmodified aarch64 Linux 5.4 kernel,
  - brings up its second core via PSCI `CPU_ON` with virtual SGIs for IPIs,
  - mounts an **ext4 root filesystem off the emulated virtio-blk disk** (`switch_root` into `/dev/vda`),
  - has **networking** (`eth0`) and can `ping` a hypervisor-emulated host,
  - reads entropy from an emulated **virtio-rng**,
  - has a **captured + interactive console** (`/proc/linux_console`),
  - (in progress) a **virtio-console** `/dev/hvc0`.
- **vCPU 3 — a small migratable guest** used to demonstrate **live migration**
  (pre-copy *and* post-copy) between physical RAM windows.

Core VMM mechanisms: per-guest stage-2 translation (distinct VMIDs), full vCPU
context switching (GP/sysreg/vGIC/FP/timer/`SP_EL0`), GICv3 virtual-interrupt
injection, trap-and-emulate, WFI idle-yield, PSCI lifecycle, and VMID-scoped TLB
maintenance.

Targets QEMU `virt` with `gic-version=3,virtualization=on`, Cortex-A72
(ARMv8.0-A, **no VHE**).

---

## 2. Capability catalog

| Area | Capability |
|---|---|
| **EL2 bring-up** | Boot detects EL2, configures the hypervisor, `eret`s Fermi to EL1 as a guest |
| **Stage-2** | Per-guest IPA→PA tables (`VTTBR`/`VTCR`), distinct VMIDs, hyp-memory unmapped from guests |
| **Isolation** | Guest access to hyp/other-guest memory faults to EL2, is poisoned + stepped |
| **Hypercalls** | SMCCC-style HVC ABI: version, paravirt console, ping, yield, introspection |
| **Trap-and-emulate** | `ESR_EL2` decode for ID-registers, GIC, PL011, virtio MMIO |
| **vGIC** | Physical IRQs → EL2 → injected as HW-linked virtual IRQs via list registers |
| **Scheduling** | EL2 timer (`CNTHP`) preemptive round-robin, 10 ms quantum |
| **WFI idle-yield** | `HCR_EL2.TWI` traps guest WFI → yield slice to other guests |
| **Context switch** | GP regs, PC/PSTATE, EL1 sysreg bank, `SP_EL0`, vGIC, FP/SIMD, virtual timer |
| **PSCI** | `VERSION`, `SYSTEM_OFF/RESET`, `CPU_ON`, `AFFINITY_INFO`, `CPU_OFF`, `FEATURES` |
| **Linux guest** | Unmodified aarch64 Linux 5.4 → BusyBox userspace |
| **SMP guest** | 2-core Linux via PSCI `CPU_ON`, per-vCPU MPIDR/redistributor/timer, virtual SGIs (IPIs) |
| **virtio-rng** | Emulated virtio-mmio entropy device (`/dev/hwrng`) |
| **virtio-blk** | Emulated virtio-mmio disk backed by an 8 MiB ext4 image → `/dev/vda` |
| **Root disk** | initramfs `switch_root`s into the ext4 root on `/dev/vda` |
| **virtio-net** | Emulated virtio-mmio NIC (`eth0`); hypervisor answers ARP + ICMP echo |
| **virtio-console** | Emulated virtio-mmio console (`/dev/hvc0`) *(in progress)* |
| **Console** | Linux console captured to `/proc/linux_console`; interactive input via UART RX + SPI injection + cursor-query terminal emulation |
| **Live migration** | Pre-copy (iterative dirty-tracking + convergence) and post-copy (demand fault-in), bidirectional |
| **Introspection** | `/proc/vms` live vCPU table (state, HVC/sysreg/abort/vIRQ/MMIO counts, world-switches, idle-yields) |

---

## 3. Branch index

Each milestone was committed and pushed to its own branch. Later branches are
generally stacked on earlier ones, so a branch contains its own work plus all
prior milestones.

| Branch | Delivers |
|---|---|
| `feat/el2-type1-hypervisor-linux-guest` | The core arc M1–M15: EL2 bring-up, stage-2, isolation, hypercalls, trap-emulate, vGIC, preemption, full context switch, FP, PSCI, the Linux guest booting to a shell, `/proc/vms` |
| `docs/hypervisor-design-20260625-173859` | `HYPERVISOR.md` design document |
| `feat/linux-init-respawn-20260625-174110` | initramfs shell respawn (exiting the shell doesn't panic init) |
| `feat/dedicated-guest-consoles-20260625-174655` | Capture the Linux console into `/proc/linux_console` |
| `feat/third-guest-nvm-20260625-180019` | A third guest (N>2 multi-VM generalization) |
| `feat/interactive-linux-console-20260625-180724` | Interactive Linux console: emulated UART RX + software SPI injection (M17) |
| `feat/virtio-rng-mmio-20260625-181521` | Emulated virtio-rng (first virtio device) |
| `feat/virtio-blk-mmio-20260625-183224` | Emulated virtio-blk (`/dev/vda`); partition scan detects `vda1` |
| `fix/pl011-rx-timeout-irq-20260625-184359` | PL011 RX-timeout interrupt status (partial; superseded) |
| `docs/devlog-20260625-185127` | `DEVLOG.md` development log |
| `fix/interactive-rx-byteloss-20260625-185320` | The real interactive-input fix: answer the line editor's `ESC[6n` cursor query |
| `test/virtio-blk-write-verify-20260625-190612` | Verify the virtio-blk write path (sector round-trip) |
| `feat/wfi-idle-yield-20260625-191905` | WFI trapping → idle guests yield their slice; `idle-yields` in `/proc/vms` |
| `feat/virtio-net-mmio-20260625-192213` | Emulated virtio-net (`eth0`); hypervisor as ARP/ICMP link peer (ping works) |
| `feat/smp-guest-2core-20260625-195648` | **SMP Linux**: 2 vCPUs via PSCI `CPU_ON`, per-core MPIDR/redistributor/timer, virtual SGIs/IPIs |
| `feat/virtio-blk-ext4-disk-20260625-195818` | Back virtio-blk with a real 8 MiB ext4 image the guest mounts |
| `feat/true-root-disk-switchroot-20260625-201728` | True root disk: initramfs `switch_root`s into `/dev/vda`; 10 ms quantum fixes SMP `stop_machine` latency |
| `feat/live-migration-precopy-20260625-203120` | **Live migration**: pre-copy (iterative dirty-tracking + convergence) and post-copy (demand fault-in), bidirectional; VMID-scoped TLBI |
| `feat/virtio-console-20260626-131717` | Emulated virtio-console (`/dev/hvc0`) — **in progress on this branch** |

Supporting/snapshot branches also exist (e.g. a `progress/…` snapshot and the
reproducible `scripts/stage-linux-guest.sh` staging work).

> Note: several pre-existing, unrelated branches (`epic/userspace`, `feat/net-*`,
> `feat/shell-*`, `feat/smp-*`, `feat/hyp-*`, `fermi-*`, etc.) exist in the repo
> from earlier work and are **not** part of this hypervisor effort.

---

## 4. Verification highlights (all observed in QEMU)

- Linux SMP: `CPU1: Booted secondary processor`, `SMP: Total of 2 processors activated`, `nproc` → 2.
- Root disk: `EXT4-fs (vda): mounted filesystem`, `[init] … switching root`, `ROOTFS_ON_VDA`.
- Networking: `64 bytes from 10.0.0.1 … 0% packet loss`.
- virtio-console: `[guest hvc0] HVC0_VIRTIO_OK` forwarded through the transmitq.
- Live migration: pre-copy dirty set converges `16 → … → 1`; post-copy demand-faults exactly the working set; the guest's counter is **monotonic across the full round trip** while the old window reads back `0xEE…` poison.
- Idle-yield: hundreds of `idle-yields` reclaimed per run via WFI trapping.

---

## 5. Key engineering lessons (see `DEVLOG.md` for detail)

- **`SP_EL0` is `current`** on arm64 Linux — saving/restoring it was *the* fix that unblocked the full Linux boot.
- **No VHE on A72** → a separate EL2 hypervisor running Fermi as an EL1 guest (not a relocation).
- **Trace before theorizing** — the interactive "byte loss" was actually the line editor's `ESC[6n` query, not lost bytes.
- **Multi-tenant TLB/CPU discipline** — global `tlbi alle1is` and a busy-spinning guest each starved co-resident guests; fixes were VMID-scoped TLBI and a WFI-yielding/retiring demo guest.
- **Time-sliced SMP is IPI-latency-sensitive** — `stop_machine` was the canary; a short (10 ms) quantum keeps cross-core IPIs responsive.

---

## 6. How to build & run

```bash
# On a host with internet (fetches an SCS-free Linux Image + builds the busybox
# initramfs + exposes busybox for the ext4 root image):
./scripts/stage-linux-guest.sh

# Inside the osdev build container (aarch64 toolchain + QEMU; project at /mnt/fermi):
make run
#   -> Fermi boots at EL2 as the hypervisor and runs Fermi + 2-core Linux
#      (+ a migratable guest). In Fermi's shell:
#        cat /proc/vms             # live vCPU table
#        cat /proc/linux_console   # the Linux guest's boot log + shell
```
