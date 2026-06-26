# FermiOS → Type-1 EL2 Hypervisor

A complete record of turning **FermiOS** — a bare-metal AArch64 EL1 kernel — into
a **type-1 (bare-metal) hypervisor running at EL2** that boots the *unmodified*
FermiOS as a guest VM, alongside a dozen smaller purpose-built guests that each
exercise one hypervisor subsystem.

> **The defining constraint:** FermiOS itself (`src/`) is **never modified**. Every
> line of hypervisor code lives in `src/hyp/`. The same kernel that ran on bare
> metal now runs one privilege level down, virtualized, with no idea it is a guest.

- **Base commit:** `a2f1104` (the original FermiOS EL1 kernel).
- **Hypervisor source:** `src/hyp/` — 57 files, ~7,800 lines of C + AArch64 asm.
- **Remote:** `git@github.com:rituparna-ui/fermi-os.git` (origin).
- **What runs today:** **14 VMs / 15 vCPUs** concurrently on one physical CPU,
  including the full 8 GiB FermiOS and an SMP guest whose secondary hot-plugs in
  and out.

---

## Table of contents

1. [How to build and run](#1-how-to-build-and-run)
2. [The load-bearing early decisions](#2-the-load-bearing-early-decisions)
3. [Architecture at a glance](#3-architecture-at-a-glance)
4. [The core hypervisor: milestones M1–M5](#4-the-core-hypervisor-milestones-m1m5)
5. [The feature chain (26 commits)](#5-the-feature-chain-26-commits)
6. [The guest menagerie (the 14 VMs)](#6-the-guest-menagerie-the-14-vms)
7. [Hypercall + management ABI](#7-hypercall--management-abi)
8. [Memory map](#8-memory-map)
9. [Register cheat-sheet (verified)](#9-register-cheat-sheet-verified)
10. [All branches involved](#10-all-branches-involved)
11. [How features were built (the process)](#11-how-features-were-built-the-process)
12. [Notable bugs caught (and how)](#12-notable-bugs-caught-and-how)

---

## 1. How to build and run

The host has no working cross-toolchain and only QEMU 3.1 (no `-cpu max`).
Everything builds and runs inside the **`osdev:dev` Docker image** (gcc 13.3 +
QEMU 8.2.2):

```bash
docker run --rm -v $PWD:/work -w /work osdev:dev bash -lc 'make all && make disk'
qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on -cpu max -m 9G \
    -nographic -kernel build/hyp.elf
```

`make` produces **two** images:

| Image | Role |
|---|---|
| `build/kernel.elf` | the **guest** (FermiOS, EL1/EL0), built from `src/` minus `src/hyp/` |
| `build/hyp.elf` | the **hypervisor** (EL2), built from `src/hyp/` |

The guest `kernel.elf` is `objcopy`'d to a flat `guest.bin` and embedded into the
hyp image via `guest_blob.S`. Each small guest (`guest2`, `ipc`, `dom0`, …) is
likewise built flat and embedded via its own `*_blob.S`. `-kernel build/hyp.elf`
is therefore the **only ROM QEMU loads** — the hyp `memcpy`s each guest to its
load address before the `eret`.

---

## 2. The load-bearing early decisions

1. **Type-1, non-VHE.** The hypervisor runs at **EL2 with `HCR_EL2.E2H=0`**; the
   guest gets the full EL1&0 regime. `-cpu max` + `virt,virtualization=on` makes
   QEMU `-kernel` enter at **EL2** — confirmed empirically before any code was
   written. (This is distinct from the VHE "run the OS itself at EL2" approach.)

2. **Hyp/guest memory split with zero guest edits.** FermiOS hardcodes 8 GiB of
   RAM at PA `0x40000000`. We launch with **`-m 9G`** and link the hypervisor at
   PA **`0x250000000`** — inside RAM but *above* the guest's 8 GiB — so the hyp
   never collides with pages the guest's PMM hands out, even before stage-2 is on.
   All guest RAM regions live in the top reserved GiB.

3. **Guest delivery by embedding, not `-device loader`.** QEMU auto-places the DTB
   at `0x40000000`, which collided with a separately-loaded guest ELF. Embedding
   each guest as a flat blob inside the hyp and `memcpy`-ing it at EL2 sidesteps
   the collision entirely.

4. **`-mgeneral-regs-only` for the hyp.** The EL2 C code emits no FP/SIMD, so the
   GPR-only 288-byte world-switch trap frame can never silently clobber a guest's
   caller-saved q-registers.

5. **Design + adversarial verification before coding.** Every non-trivial feature
   was first *designed* and then *attacked* by a multi-agent workflow (3 hostile
   review lenses), and the riskier ones were code-reviewed against the spec after
   implementation. This caught register-level bugs that "work" in a demo but
   corrupt silently later — see [§12](#12-notable-bugs-caught-and-how).

---

## 3. Architecture at a glance

```
        EL2 (Hypervisor)                       guest sees:
  ┌───────────────────────────────┐
  │ hyp_boot.S   EL2 reset + eret  │
  │ hyp_vectors.S  trap frame      │      ┌──────────────────────────┐
  │ vm.c         trap dispatcher   │◄─────┤ HVC / SMC / WFI           │
  │ vcpu.c       scheduler + ctx   │◄─────┤ data abort (MMIO trap)    │
  │ stage2.c     per-VM stage-2    │      │ trapped sysreg (CNTP_*,   │
  │ vgic/        virtual GICv3     │      │   ICC_SGI1R, ICC_PMR)     │
  │ timer/       virtual timer     │      └──────────────────────────┘
  │ virtio/      rng/blk/net/balloon
  │ vpci/        PCI bridge + MSI-X │
  │ snapshot.c   checkpoint/migrate │
  └───────────────────────────────┘
        │ world switch (eret)
        ▼
  ┌────────────────────────────────────────────────────────────────┐
  │ EL1/EL0 guests, each in its own stage-2 (distinct VTTBR + VMID)  │
  │  FermiOS(8GiB) · guest2 · ipc×2 · dom0 · vmtgt · crasher ·       │
  │  hangguest · rng/blk/net/pci clients · smp(2 vCPU) · balloon     │
  └────────────────────────────────────────────────────────────────┘
```

Everything at EL2 runs **MMU-off** (Normal Non-cacheable), so all hyp addresses
are physical and any access to a cacheable guest's memory needs explicit cache
maintenance (`dc civac` before reading guest writes, `dc cvac` after writing).

**Trap-and-emulate dispatch** (`vm.c::hyp_dispatch`): on every exception from a
guest, `hyp_vectors.S` builds the trap frame and calls into C. Synchronous traps
route by exception class — HVC/SMC → PSCI + vendor hypercalls; trapped sysreg →
vtimer / ICC_SGI1R routing / ICC_PMR; WFx → block-and-switch; data abort →
whichever emulated MMIO device owns the faulting IPA window. Physical IRQs (timer)
route to the scheduler.

---

## 4. The core hypervisor: milestones M1–M5

Each milestone is independently buildable and was verified in QEMU serial output.

### M1 — Boot at EL2, `eret` into unchanged FermiOS (`bd93e5e`)
`hyp_boot.S` is entered at EL2; it sets the minimal EL2 context and `eret`s
straight into FermiOS at EL1 with **no stage-2 yet** (`HCR_EL2.VM=0`, IPA==PA).
Proves the OS boots one level down, unchanged. Key correction baked in:
`CPTR_EL2=0x32FF` (the "obvious" `0x33FF` leaves SVE trapped — a hang).

### M2 — Stage-2 translation (`4f8844f`)
`stage2.c`. `HCR_EL2.VM=1` with an identity IPA→PA map: 40-bit IPA, 4 KiB granule,
SL0=1 ⇒ a concatenated 1024-entry L1, `VTCR_EL2=0x80023558`. Stage-2 descriptors
differ from stage-1: `S2AP_RW=3<<6` (stage-1's `0<<6` means *no access* at
stage-2), memory type is **direct** in `MemAttr[5:2]` (no MAIR), `XN[54:53]`.
GICD/GICR left invalid so they trap (for M5).

### M3+M4 — World-switch spine, virtual GICv3, virtual timer (`ffd5391`)
- **World switch:** `hyp_vectors.S` builds a 288-byte GPR-only trap frame on
  `SP_EL2`, saves EL2-banked syndrome regs, dispatches, restores, `eret`s.
- **Virtual timer** (`timer/vtimer.c`): the guest's EL1 physical timer is
  *level-triggered* — pure passthrough storms EL2. So **trap** guest `CNTP_*`,
  drive the **EL2 physical timer `CNTHP`** to the guest's deadline, and inject a
  virtual INTID 30 on the CNTHP PPI. The guest's `timer.c` is unchanged.
- **Virtual GICv3** (`vgic/vgic.c`): with `HCR_EL2.IMO=1`, guest `ICC_*_EL1`
  CPU-interface accesses are **hardware-redirected** to the virtual interface (no
  per-ack/EOI trap). The hyp enables `ICH_HCR_EL2.En`, seeds
  `ICH_VMCR_EL2=0xFF000002`, reads the List-Register count from `ICH_VTR_EL2`
  (QEMU reports **4**, not 16), and injects via a free `ICH_LR<n>_EL2`.
- Final `HCR_EL2=0x80082039` (RW|VM|FMO|IMO|AMO|TWI|TSC).

### M5 — GICD/GICR MMIO trap-and-emulate (`ab6cb9b`)
Leave the GIC distributor/redistributor stage-2-invalid so guest MMIO faults to
EL2 (`EC=0x24`); a small software vGIC model services it (GICD_CTLR, GICR_WAKER
returning ChildrenAsleep=0, ISENABLER, …). IPA reconstructed from
`HPFAR_EL2[39:4] | FAR_EL2[11:0]`, access decoded from the ESR ISS. **FermiOS is
now fully virtualized** — stage-2 memory, virtual GIC (MMIO + CPU interface),
virtual timer — and runs to its interactive shell.

---

## 5. The feature chain (26 commits)

After M1–M5 + a docs/hardening pass, each subsequent feature is one commit on its
own branch, stacked on the prior. Listed oldest → newest:

| Commit | Feature | One-line summary |
|---|---|---|
| `bd93e5e` | M1 | boot at EL2, eret into unchanged FermiOS |
| `4f8844f` | M2 | stage-2 translation (HCR_EL2.VM=1) |
| `ffd5391` | M3+M4 | world-switch spine + vGICv3 + vtimer |
| `ab6cb9b` | M5 | GICD/GICR MMIO trap-and-emulate |
| `3bce189` | **multi-VM** | 2nd guest + EL2 round-robin scheduler, per-VM stage-2 isolation |
| `e77b544` | docs | architecture README |
| `10ee43f` | hardening | 9 correctness bugs from a 5-dimension adversarial review |
| `1d16801` | **fair sched** | block-on-WFI; idle guest woken on its own timer |
| `d30a6a9` | **PSCI lifecycle** | per-VM warm reset + power off |
| `c6772ae` | **inter-VM shmem** | producer/consumer share one host page at a common IPA |
| `3e8fa4c` | **doorbell** | event-channel notification (inject SPI 40, wake peer) |
| `c3fe382` | **dom0** | privileged control domain + VMCTL management hypercall |
| `b6817b3` | **observability** | per-VM exit accounting (xentop-style) via VMCTL_STAT |
| `f1527ec` | **PV console** | HVC_FERMI_LOG with bounds-checked IPA→PA translation |
| `261530a` | **weighted sched** | proportional-share slices + CPU-time accounting |
| `396632d` | **snapshot/restore** | full-state checkpoint + rollback to a reserved slot |
| `6bd2d68` | **live migration** | clone a snapshot into a different VM slot |
| `9c990cd` | **fault isolation** | reboot/kill a faulting guest, not the box |
| `05f9046` | **watchdog** | reboot a hung (livelocked) guest |
| `cd26d84` | **virtio-rng** | virtio-mmio entropy device (standard PV transport) |
| `f497991` | **virtio-blk** | virtio-mmio block device with read/write |
| `edade08` | **virtio-net** | virtio-mmio two-queue loopback NIC |
| `46c4569` | **vPCI** | virtual PCI host bridge over ECAM + BAR sizing |
| `2108588` | **SMP guests** | multi-vCPU VMs: PSCI CPU_ON + software SGI routing |
| `79c72db` | **virtio-balloon** | inflate/deflate of donated guest PFNs |
| `06ac871` | **PSCI CPU_OFF** | secondary-vCPU hotplug (online→offline→online) |
| `37b7f25` | **MSI-X** | message-signaled interrupts on the vPCI device + mask/PBA |

### What each feature actually does

**multi-VM + scheduler** — `vcpu.c`/`vcpu.h`/`vcpu_switch.S`. A full per-vCPU
context (`struct vcpu`): GP regs, the complete EL1 sysreg set FermiOS uses, FP
(q0–q31), per-VM vGIC, per-VM vtimer shadow, per-VM stage-2 root (distinct VTTBR +
VMID). Two guests at the same IPA `0x40000000` map to **different host PAs** — true
isolation. An EL2 round-robin scheduler world-switches on the CNTHP PPI.

**fair scheduler** — an idle guest (WFI) yields and is woken precisely on its own
timer; `hyp_cnthp_arm` folds in *every* vCPU's vtimer deadline. Fixed a real
starvation problem where a spin-heavy guest starved an idle one.

**PSCI lifecycle** — the guest's own `hvc` (PSCI) drives per-VM **warm reset**
(reload the pristine image + re-init state + restart) and **power off** — a per-VM
operation, never a machine reset.

**inter-VM shared memory** — the inverse of isolation: the hyp *grants* a shared
page by mapping one host page into two VMs' stage-2 at a common IPA (`0x50000000`).
Producer/consumer guests exchange a sequence number while their private RAM stays
isolated (Xen-grant-table / KVM-ivshmem).

**doorbell / event channel** — `HVC_FERMI_DOORBELL` injects a virtual SPI into the
peer and wakes it, making the consumer event-driven (WFI between events).

**dom0 management plane** — a privileged control guest issues the **VMCTL**
hypercall against other VMs (COUNT/STATE/RUNS/RESET/STOP/START/…). Non-privileged
VMs get `VMCTL_EPERM`. Xen-dom0 / libvirt-`virsh`.

**observability** — per-VM exit counters by reason (HVC, data-abort, sysreg, WFx,
IRQ) via `VMCTL_STAT`. The profiles fingerprint each VM (dom0 all-HVC; the
event-driven consumer is WFx-heavy with *zero* IRQ exits — its doorbells are
HW-delivered via List Registers and never trap EL2).

**PV console** — `HVC_FERMI_LOG` translates a guest buffer IPA→host PA,
**bounds-checked** to the VM's RAM window, and prints it tagged `[vmname]`.

**weighted scheduler** — each vCPU has a `weight`; slice = `base × weight`
(Xen-credit / cgroup `cpu.weight`). `VMCTL_WEIGHT` / `VMCTL_CPUTIME` expose it.

**snapshot/restore** — `VMCTL_SNAPSHOT`/`RESTORE` checkpoint a guest's full state
+ private RAM into a reserved slot and roll it back. Load-bearing points: `vtimer.
cval` stored *relative* and rebased (an absolute deadline would storm); restore
flushes the *target* VMID's TLB via a temporary `VTTBR_EL2` swap; capture reads
only from the `vcpu_t` struct (never the current vCPU).

**live migration** — `VMCTL_MIGRATE` transplants a snapshot into a *different* VM
slot, which resumes in the destination's own stage-2 (different host PA + VMID).
Works because guest state is **host-PA / VMID agnostic** (only IPAs + virtual
INTIDs). After migrate, two beat streams run — the original and its clone.

**fault isolation** — an unhandled guest trap reboots only that VM (warm reset)
or, past a fault budget (`VCPU_FAULT_MAX=3`), powers it off — never panics the
box. EL2-self traps still panic (they are real hypervisor bugs).

**watchdog** — a guest arms `HVC_FERMI_WDOG` and must pet it; if it stops
(livelock without faulting), the scheduler reboots it. Catches hangs the way fault
isolation catches crashes.

**virtio-rng / blk / net / balloon** — four emulated virtio-mmio (modern, Version 2)
devices, each at a distinct stage-2-invalid IPA window so accesses trap to EL2. A
shared split-virtqueue walk (`virtq_desc` 16B; avail/used rings) + cache-coherence
discipline + used.idx publish ordering underlies all of them:
- **rng** (DeviceID 4, SPI 41): fills WRITE descriptors with PRNG bytes.
- **blk** (DeviceID 2, SPI 42): a RAM-backed disk with bidirectional read/write.
- **net** (DeviceID 1, SPI 43): two virtqueues (RX/TX), a loopback NIC.
- **balloon** (DeviceID 5, SPI 44): two queues (inflate/deflate) carrying arrays
  of device-READ PFNs. *Honest model:* the fixed stage-2 has no runtime unmap, so
  "inflation" **zeroes** each donated page (proving legitimate reuse) and counts
  it — logged `NOT host-unmapped`; it never overclaims. Self-driven by a CNTPCT
  retarget clock exercising both inflate and deflate.

**vPCI** — a virtual PCI host bridge over an ECAM window: a guest enumerates the
bus, sizes + assigns a BAR (the classic write-`0xFFFFFFFF` / read-size-mask
probe), and enables the device.

**SMP guests** — a VM can have >1 vCPU. Siblings share one stage-2 (same
VTTBR/VMID — real shared memory) and a `group_id`, but each has a distinct MPIDR
(`VMPIDR_EL2` reloaded every world switch). `PSCI CPU_ON` brings a secondary up;
inter-processor SGIs (`ICC_SGI1R_EL1`, trapped via `ICH_HCR_EL2.TC` **per-SMP-vCPU
only**) are software-routed to the target sibling. `ICC_PMR_EL1` (also caught by
TC) is emulated to `ICH_VMCR_EL2.VPMR`.

**PSCI CPU_OFF + hotplug** — a secondary powers *itself* down (`online=0`, not
dead) and a later `CPU_ON` brings it back: the full online→offline→online cycle.
Restricted to SMP secondaries (a primary/UP VM must use SYSTEM_OFF). One
load-bearing rule: CPU_OFF returns nothing on success and has already switched
away, so the handler must **not** write `x0` then. `hyp_cnthp_arm` gained a
mandatory `online` gate (else an offlined secondary's stale vtimer livelocks
CNTHP).

**MSI-X** — message-signaled interrupts on the vPCI device. A 2-vector table + PBA
live in a fixed-base BAR1 trapping window; the guest programs entries, unmasks, and
enables MSI-X, then rings a doorbell register and the hyp injects the vINTID.
Full mask/PBA/deferred-delivery state machine: ring-while-masked sets the PBA bit
and injects nothing; on any delivery-enabling edge a single PBA consumer delivers
and clears the bit *only on confirmed enqueue* (no lost interrupt, no double-fire).
Defense-in-depth: the SPI is clamped to the device's range, and a separate
`vgic_inject_spi_try` independently rejects anything outside 32..1019 (kept
separate from `vgic_inject_ppi` so the vtimer PPI 30 path is unaffected).

---

## 6. The guest menagerie (the 14 VMs)

Each small guest is a self-contained flat EL1 image (`src/hyp/<name>/<name>.S`)
linked at `0x40000000`, embedded via a `*_blob.S`, that exercises exactly one
hypervisor subsystem. `id`/`VMID` are assigned in `hyp.c`.

| id | VM | Demonstrates |
|---|---|---|
| 0 | **FermiOS** | the unmodified 8 GiB EL1 kernel, fully virtualized |
| 1 | guest2 | heartbeat printer (multi-VM world switch, PV console) |
| 2 | ipc-prod | inter-VM shared memory (producer role) |
| 3 | ipc-cons | inter-VM shared memory + doorbell (event-driven consumer) |
| 4 | dom0 | privileged control domain (VMCTL management plane) |
| 5 | vmtgt | live-migration destination (idle stub) |
| 6 | crasher | fault isolation (deliberately dereferences an unmapped IPA) |
| 7 | hangguest | liveness watchdog (pets a few times, then livelocks) |
| 8 | rngclient | virtio-mmio entropy device |
| 9 | blkclient | virtio-mmio block device (write+read round-trip) |
| 10 | netclient | virtio-mmio loopback NIC |
| 11 | pciclient | vPCI enumeration + BAR sizing **+ MSI-X** (immediate/mask/deferred) |
| 12 | smp-cpu0 | SMP primary (CPU_ONs the secondary, drives the hotplug cycle) |
| 13 | smp-cpu1 | SMP secondary (ping-pongs SGIs, then CPU_OFFs itself) |
| 14 | balloonclient | virtio-mmio memory balloon (inflate/deflate) |

`smp-cpu0` and `smp-cpu1` are sibling vCPUs of **one** VM (shared stage-2), so the
roster is 14 VMs across 15 vCPUs; the secondary hot-plugs in and out at runtime.

---

## 7. Hypercall + management ABI

Vendor hypercalls (id in `x0`, outside the PSCI range):

| HVC id | Name | Meaning |
|---|---|---|
| `0xFE110000` | `HVC_FERMI_YIELD` | yield the rest of the time slice |
| `0xFE110001` | `HVC_FERMI_DOORBELL` | notify the peer VM (inject doorbell IRQ) |
| `0xFE110002` | `HVC_FERMI_VMCTL` | management op (privileged dom0): x1=op, x2=target, x3=arg |
| `0xFE110003` | `HVC_FERMI_LOG` | PV console: x1=buf IPA, x2=len |
| `0xFE110004` | `HVC_FERMI_WDOG` | arm/pet the liveness watchdog (x1=timeout ticks, 0=disarm) |

**VMCTL ops** (in x1): `COUNT`, `STATE`, `RUNS`, `RESET`, `STOP`, `START`, `STAT`,
`WEIGHT`, `CPUTIME`, `SNAPSHOT`, `RESTORE`, `MIGRATE`.

**PSCI** (the guest's `hvc`/`smc`): `VERSION`, `SYSTEM_OFF`, `SYSTEM_RESET`,
`CPU_ON` (SMC64 `0xC4000003`), `CPU_OFF` (`0x84000002`), `AFFINITY_INFO`
(`0xC4000004`), `FEATURES`.

**Virtual interrupt map (SPIs):** 40 doorbell, 41 rng, 42 blk, 43 net, 44 balloon,
45/46 MSI-X vectors. Timer is virtual INTID 30 (guest EL1 phys timer).

---

## 8. Memory map

QEMU `-m 9G` ⇒ RAM `0x40000000 .. 0x280000000`.

| Region | Host PA | Notes |
|---|---|---|
| VM1 (FermiOS) RAM | `0x40000000 .. 0x240000000` (8 GiB) | the guest's hardcoded `MEM_SIZE` |
| Hypervisor image + pool | `0x250000000 ..` | text/data/stack + bump allocator |
| VM2 RAM | `0x260000000` (64 MiB) | heartbeat guest |
| IPC producer / consumer | `0x264000000` / `0x265000000` | + shared page `0x266000000` |
| dom0 / vmtgt / crasher / hangguest | `0x268000000 ..` | one 16–64 MiB region each |
| rng / blk / net / pci clients | `0x272000000 .. 0x276000000` | 16 MiB each |
| SMP guest RAM | `0x276000000` (16 MiB) | **both** vCPUs share it (one stage-2) |
| balloon client RAM | `0x277000000` (16 MiB) | |

**Emulated MMIO windows** (stage-2-invalid → trap to EL2): GICD `0x08000000`,
GICR `0x080A0000`, virtio-rng `0x0A000000`, virtio-blk `0x0A001000`, virtio-net
`0x0A002000`, vPCI ECAM `0x0A003000`, virtio-balloon `0x0A004000`, MSI-X table/PBA
`0x0A005000`.

The hyp links *above* the guest's 8 GiB so it never collides with pages the
guest's PMM hands out, even with stage-2 off.

---

## 9. Register cheat-sheet (verified)

| Register | Value | Meaning |
|---|---|---|
| `CPTR_EL2` | `0x32FF` | don't trap guest FP/SIMD/SVE (`0x33FF` traps SVE — a hang) |
| `SPSR_EL2` (guest entry) | `0x3C5` | EL1h + DAIF masked |
| `HCR_EL2` (final) | `0x80082039` | RW\|VM\|FMO\|IMO\|AMO\|TWI\|TSC |
| `VTCR_EL2` | `0x80023558` | 40-bit IPA, SL0=1, 4K granule, 8-bit VMID |
| `CNTHCTL_EL2` | `0x1` | allow guest CNTPCT reads, trap CNTP_* |
| `ICC_SRE_EL2` | `0xF` | Enable\|DIB\|DFB\|SRE |
| `ICH_VMCR_EL2` | `0xFF000002` | VPMR=0xFF, VENG1=1, VEOIM=0 |
| `ICH_HCR_EL2` | `En` (+`TC` for SMP vCPUs) | virtual interface on; TC traps ICC_SGI1R/PMR |
| stage-2 S2AP | `3<<6` (RW) | NOT stage-1 `PTE_AP_RW` (=0 = no-access) |
| MPIDR (vCPU) | `0x80000000` / `…01` | bit31 RES1, U=0; Aff0 = vCPU index |
| HVC/SMC ELR | no advance | only aborts/sysreg/WFx advance `ELR_EL2` by 4 |

---

## 10. All branches involved

All on `git@github.com:rituparna-ui/fermi-os.git`. Each branch is a self-contained,
uniquely-named PR; the chain stacks (each built on the prior). Verified
`local HEAD == remote ref` at push time for every one.

| # | Branch | Tip | Adds |
|---|---|---|---|
| — | `feat/el2-hypervisor-fermios-guest` | `c3fe382` | core hypervisor: M1–M5, multi-VM, fair sched, PSCI lifecycle, IPC, doorbell, dom0 |
| 1 | `feat/hyp-observability` | `b6817b3` | VMCTL_STAT exit accounting |
| 2 | `feat/hyp-pv-console` | `f1527ec` | HVC_FERMI_LOG PV console |
| 3 | `feat/hyp-weighted-sched` | `261530a` | weighted scheduler + VMCTL_WEIGHT/CPUTIME |
| 4 | `feat/hyp-snapshot-restore` | `396632d` | VM checkpoint / rollback |
| 5 | `feat/hyp-live-migration` | `6bd2d68` | live migration (clone) |
| 6 | `feat/hyp-fault-isolation` | `9c990cd` | per-VM fault isolation |
| 7 | `feat/hyp-watchdog` | `05f9046` | per-VM liveness watchdog |
| 8 | `feat/hyp-virtio-mmio` | `cd26d84` | virtio-mmio entropy device |
| 9 | `feat/hyp-virtio-blk` | `f497991` | virtio-mmio block device |
| 10 | `feat/hyp-virtio-net` | `edade08` | virtio-mmio loopback NIC |
| 11 | `feat/hyp-vpci` | `46c4569` | virtual PCI bus (ECAM + BAR sizing) |
| 12 | `feat/hyp-smp-guest` | `2108588` | SMP guests (PSCI CPU_ON + SGI routing) |
| 13 | `feat/hyp-virtio-balloon` | `79c72db` | virtio-mmio memory balloon |
| 14 | `feat/hyp-psci-cpu-off` | `06ac871` | PSCI CPU_OFF + secondary hotplug |
| 15 | `feat/hyp-vpci-msix` | `37b7f25` | MSI-X on the vPCI device |

> Note: the repo also contains many *non-hypervisor* `feat/*` branches (FAT32,
> shell commands, networking, an earlier SMP-for-FermiOS effort, a separate VHE
> hypervisor variant, a Linux-guest variant, etc.). Those predate or run parallel
> to this effort; the table above is the **type-1 EL2 hypervisor** chain only.

---

## 11. How features were built (the process)

The riskier features followed a deliberate three-phase loop:

1. **Design + adversarial verify (before any code).** A multi-agent workflow:
   one design agent produces a complete, code-grounded spec; then 3 *hostile*
   review agents each attack it from a distinct lens (e.g. spec-correctness,
   security/VM-escape, cache-coherence/ordering); a synthesizer folds every
   confirmed bug into a final spec. This caught register-level defects *before*
   they were written — e.g. the MSI-X reviews unanimously proposed gating INTIDs
   inside `vgic_inject_ppi`, which the synthesizer caught would **break the vtimer
   PPI 30** (a separate gated function was used instead).

2. **Implement to the spec**, building inside the `osdev:dev` container.

3. **Verify + code-review.** Run in QEMU and confirm the feature's specific
   evidence *and* zero regression across all other VMs (FermiOS still boots, dom0
   still enumerates the full roster, no new panics/traps). For the riskier
   features, a code-review agent then checks the *implementation* against the spec
   invariant-by-invariant.

Only after a feature builds clean and verifies in QEMU is it committed to its own
branch and pushed, with a check that `local HEAD == remote ref`.

---

## 12. Notable bugs caught (and how)

A sampling of the defects this process surfaced — most would "work" in a demo but
corrupt or hang later:

- **`CPTR_EL2=0x33FF` traps SVE** → boot hang. The design review flagged the RES1
  value before M1 was coded; `0x32FF` is correct.
- **stage-2 `S2AP`** — reusing stage-1's `PTE_AP_RW` (`0<<6`) means *no access* at
  stage-2; every guest access would fault. Caught in the M2 design review.
- **vtimer cleared `CNTP_CTL.ENABLE` on fire** → the guest's IRQ handler read the
  timer as disabled when checking ISTATUS. Now a per-vCPU `pending` flag latches
  ISTATUS. (One of 9 from the post-M5 adversarial review.)
- **fault-isolation fall-through** — making `hyp_fatal_trap` return (instead of
  panic) meant `handle_data_abort` kept emulating against the *rebooted* VM's
  fresh frame, corrupting it. Fix: `return` after each fault-isolating call.
  Diagnosed by bisecting against the parent commit.
- **doorbell IRQ handler clobbered the IAR** (`x9`, the UART register) before EOI
  → EOI'd a garbage INTID → List Register stuck Active → only one event delivered.
- **global `ICH_HCR_EL2.TC` regressed every guest** — TC traps the *whole* common
  ICC group including `ICC_PMR_EL1`, so seeding it for all vCPUs faulted FermiOS
  during GIC bring-up. Fix: TC is per-vCPU, SMP-only, and `ICC_PMR_EL1` is
  emulated.
- **CPU_OFF write-after-switch** — writing the PSCI return value to `x0` after the
  successful switch-away would clobber the *next* vCPU's register; and a missing
  `online` gate in `hyp_cnthp_arm` would livelock CNTHP on an offlined secondary's
  stale timer. Both caught by the design panel.
- **MSI-X INTID gate placement** — see §11; gating `vgic_inject_ppi` would have
  broken the vtimer.

---

*This document reflects the tree at `feat/hyp-vpci-msix` (`37b7f25`). The
companion `src/hyp/README.md` holds the deeper per-subsystem design notes;
`PROJECT_LOG.md` is the earlier (M1–fault-isolation) narrative log.*
