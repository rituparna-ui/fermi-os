# FermiOS → Type-1 EL2 Hypervisor — Project Log

A complete record of converting **FermiOS** (a bare-metal AArch64 EL1 kernel)
into a **type-1 hypervisor at EL2** that runs the *unmodified* FermiOS — plus
several smaller guests — as virtual machines.

- **Repo / branch base:** `a2f1104` (the original FermiOS EL1 kernel).
- **All hypervisor code lives in `src/hyp/`.** FermiOS itself (`src/`) is
  **never modified** — it runs as a guest exactly as it was.
- **Build/run env:** the host has no working cross-toolchain and only QEMU 3.1.
  Everything builds/runs inside the `osdev:dev` Docker image (gcc 13.3 + QEMU
  8.2.2, which has `-cpu max`). Example:
  ```
  docker run --rm -v $PWD:/work -w /work osdev:dev bash -lc 'make && make disk'
  qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on -cpu max -m 9G \
      -nographic -kernel build/hyp.elf
  ```

---

## Why this shape (the load-bearing early decisions)

1. **Type-1, not VHE.** The goal was a true bare-metal hypervisor running
   FermiOS as a guest. We run the hypervisor **non-VHE at EL2** (`HCR_EL2.E2H=0`)
   and the guest in the EL1&0 regime. `-cpu max` + `virt,virtualization=on` makes
   QEMU `-kernel` enter at **EL2** — confirmed empirically before any code.

2. **Hyp/guest memory split with zero guest edits.** FermiOS hardcodes 8 GiB of
   RAM at PA `0x40000000`. We launch QEMU with **`-m 9G`** and link the
   hypervisor at PA **`0x250000000`** — inside RAM but *above* the guest's 8 GiB
   — so the hyp never collides with pages the guest's PMM hands out, even before
   stage-2 is on. The guests' RAM regions live in the top reserved GiB.

3. **Guest delivery by embedding, not `-device loader`.** QEMU auto-places the
   DTB at `0x40000000`, which collided with a separately-loaded guest ELF. So
   each guest image is **embedded as a flat blob inside the hyp image** and
   `memcpy`'d to its load address at EL2. `-kernel build/hyp.elf` is the only ROM.

4. **`-mgeneral-regs-only` for the hyp.** Guarantees the EL2 C code emits no
   FP/SIMD, so the GPR-only world-switch trap frame can never silently clobber a
   guest's caller-saved q-registers.

5. **Adversarial verification up front.** The riskiest features (the EL2 design
   itself, and snapshot/restore) were designed and **adversarially reviewed by a
   multi-agent workflow before coding**. That caught real bugs that "work" in a
   demo but corrupt silently later (see below).

---

## The build: two images

`make` produces **two** ELF images:
- `build/kernel.elf` — the **guest** (FermiOS, EL1/EL0), built from `src/` minus
  `src/hyp/`.
- `build/hyp.elf` — the **hypervisor** (EL2), built from `src/hyp/` (excluding
  the standalone guest sub-dirs, which are built to flat blobs and embedded).

The guest `kernel.elf` is objcopy'd to a flat `guest.bin` and embedded in the
hyp via `guest_blob.S`. The small guests (`guest2`, `ipc`, `dom0`, `vmtgt`,
`crasher`) are each built flat and embedded the same way.

---

## Milestones M1–M5: the core hypervisor

Each milestone is independently buildable and was verified in QEMU serial output.

### M1 — Boot at EL2, `eret` into unchanged FermiOS at EL1 (`bd93e5e`)
The smallest real step. `src/hyp/hyp_boot.S` is entered at EL2; it sets up the
minimal EL2 context and `eret`s straight into FermiOS at EL1 with **no stage-2
yet** (`HCR_EL2.VM=0`, IPA==PA passthrough). Proves the OS boots one level down,
unchanged.

Key registers (verified, with corrections from the design review baked in):
- `CPTR_EL2 = 0x32FF` — do **not** trap guest FP/SIMD/SVE. (`0x33FF`, the
  "obvious" RES1 value, leaves SVE trapped — a hang. This was a caught bug.)
- `SPSR_EL2 = 0x3C5` — EL1h + DAIF masked, for the eret into the guest.
- `MDCR_EL2` — `HPMN = PMCR_EL0.N`, no debug/PMU traps.
- `VPIDR_EL2`/`VMPIDR_EL2` = real MIDR/MPIDR. Never touch `SCR_EL3` (no EL3 on
  this machine; access would UNDEF).

### M2 — Stage-2 translation (`4f8844f`)
`src/hyp/stage2.c`. Turn on `HCR_EL2.VM=1` with an identity IPA→PA stage-2 map.
40-bit IPA, 4 KiB granule, SL0=1 ⇒ a **concatenated 1024-entry L1** (8 KiB
aligned). `VTCR_EL2 = 0x80023558`.

Stage-2 descriptor encodings differ from stage-1 (caught bugs):
- `S2AP_RW = 3<<6` — stage-1's `PTE_AP_RW` is `0<<6`, which means **no access**
  at stage-2 (every guest access would fault).
- Memory type is **direct** in `MemAttr[5:2]` (Normal-WB=`0b1111`,
  Device-nGnRE=`0b0001`) — there is **no MAIR** at stage-2.
- `XN[54:53]` is a 2-bit enumerated field.

Guest RAM → Normal-WB; UART/PCI windows → Device-nGnRE; GICD/GICR left invalid
to trap (for M5).

### M3+M4 — World-switch spine, virtual GICv3, virtual timer (`ffd5391`)
- **World switch:** `hyp_vectors.S` builds a 288-byte GPR-only trap frame on
  `SP_EL2`, saves EL2-banked syndrome regs, calls `hyp_dispatch`, restores, eret.
- **Virtual timer** (`timer/vtimer.c`): the guest drives the **EL1 physical
  timer** (`CNTP_*`), which is **level-triggered** — pure passthrough storms EL2.
  Correct design: **trap** guest `CNTP_*` (`CNTHCTL_EL2.EL1PCEN=0`), drive the
  **EL2 physical timer** `CNTHP` to the guest's deadline, and on the CNTHP PPI
  inject a **virtual INTID 30** into the guest. No storm; `timer.c` unchanged.
- **Virtual GICv3** (`vgic/vgic.c`): with `HCR_EL2.IMO=1`, the guest's
  `ICC_*_EL1` CPU-interface accesses are **hardware-redirected** to the virtual
  interface — no per-ack/EOI trap. The hyp enables the virtual interface
  (`ICH_HCR_EL2.En`), seeds `ICH_VMCR_EL2=0xFF000002`, reads the List-Register
  count from `ICH_VTR_EL2` (QEMU reports **4**, not 16 — read it, don't assume),
  and injects via a free `ICH_LR<n>_EL2`.
- Final `HCR_EL2 = 0x80082039` (RW|VM|FMO|IMO|AMO|TWI|TSC).
- The verifier corrected a real ABI bug here: **HVC and SMC are symmetric** —
  neither advances `ELR_EL2` (only aborts/sysreg/WFx need +4).

### M5 — GICD/GICR MMIO trap-and-emulate (`ab6cb9b`)
Leave the GIC distributor/redistributor stage-2-invalid so guest MMIO faults to
EL2 (`EC=0x24`), and service it with a small software vGIC model (GICD_CTLR,
GICR_WAKER returning ChildrenAsleep=0, ISENABLER, etc.). The IPA is reconstructed
from `HPFAR_EL2[39:4]` + `FAR_EL2[11:0]`; the access is decoded from the ESR ISS
(ISV=1 on QEMU for the guest's str/ldr). The guest is now **fully virtualized**.

---

## Hardening & docs

- **`e77b544`** — `src/hyp/README.md`: architecture doc (boot flow, memory map,
  per-subsystem design, register cheat-sheet).
- **`10ee43f`** — an adversarial review (5 dimensions, each finding
  independently verified) confirmed **9 real bugs**, all fixed. The critical one:
  the vtimer cleared `CNTP_CTL.ENABLE` on fire, so the guest's IRQ handler read
  the timer as *disabled* when checking ISTATUS → latched a `pending` flag
  instead. Others: IMASK handling, a CNTHP re-arm storm guard, sub-32-bit MMIO
  masking, the missing stage-2 TLB flush on `HCR_EL2.VM` 0→1, and removing the
  read-only `ESR/FAR/PAR_EL1` from the saved context.

---

## Multi-VM + the operational suite

### Multi-VM + round-robin scheduler (`3bce189`)
`vcpu.c`/`vcpu.h`/`vcpu_switch.S`. A full per-vCPU context (`struct vcpu`): GP
regs, the complete EL1 sysreg set FermiOS uses, FP (q0–q31), per-VM vGIC state,
per-VM vtimer shadow, and a per-VM stage-2 root (distinct VTTBR+VMID). A second
guest (`guest2/`) runs alongside FermiOS. Both believe they run at IPA
`0x40000000` but map to **different host PAs** — true memory **isolation**.

### Fair scheduler — block-on-WFI (`1d16801`)
An idle guest (WFI) yields the CPU and is woken precisely on its own timer
(CNTHP folds in **every** vCPU's vtimer deadline). Fixed a real fairness problem
where a spin-heavy guest starved an idle one.

### PSCI VM lifecycle (`d30a6a9`)
Per-VM **warm reset** (reload pristine image + re-init state, restart) and
**power-off**, driven by the guest's own `hvc` (PSCI). A per-VM operation, not a
machine reset.

### Inter-VM shared memory (`c6772ae`)
The inverse of isolation: the hyp **grants** a shared page by mapping one host
page into two VMs' stage-2 at a common IPA (`0x50000000`). Producer/consumer
guests (`ipc/`, one image, role via `x0`) exchange a sequence number through it
while their private RAM stays isolated. Xen-grant-table / KVM-ivshmem model.

### Inter-VM doorbell / event channel (`3e8fa4c`)
Makes the consumer **event-driven**: the producer rings `HVC_FERMI_DOORBELL`, the
hyp injects a virtual IRQ (SPI 40) into the peer and wakes it. The consumer sets
up its own GICv3 interface + EL1 IRQ vector and WFIs between events.
*Debugging win:* the IRQ handler clobbered `x9` (the IAR value) in the UART
helpers before EOI → EOI'd a garbage INTID → List Register stuck Active → only
one event delivered. Stashing the IAR fixed it.

### dom0-style management plane (`c3fe382`)
A privileged control guest (`dom0/`) that may issue the **`VMCTL` hypercall**
(`HVC 0xFE110002`) against other VMs: `COUNT`, `STATE`, `RUNS`, `RESET`, `STOP`
(pause), `START`. Non-privileged VMs get `VMCTL_EPERM`. A paused VM is skipped by
the scheduler and not auto-resumed by its timer. Xen-dom0 / libvirt-`virsh`.

### Exit accounting / observability (`b6817b3`)
Per-VM exit counters by reason (HVC, data-abort, sysreg, WFx, IRQ), surfaced via
`VMCTL_STAT`. The profiles fingerprint each VM: FermiOS is sysreg/IRQ-heavy, dom0
is all-HVC, the event-driven IPC consumer is WFx-heavy with **zero IRQ exits**
(its doorbells are hardware-delivered via List Registers and never trap to EL2).

### Paravirtualized console (`f1527ec`)
`HVC_FERMI_LOG` (buf IPA + len): the hyp translates the guest IPA→host PA,
**bounds-checked** against the VM's RAM window (so a guest can't make the hyp
read arbitrary host memory), and prints it tagged `[vmname]`. virtio-console /
Xen-console model.

### Weighted proportional-share scheduler (`261530a`)
Each vCPU has a `weight`; its time slice is `base * weight` (Xen-credit /
cgroup-`cpu.weight`). dom0 sets shares via `VMCTL_WEIGHT` and reads consumed CPU
time via `VMCTL_CPUTIME` (per-VM CNTPCT ticks). Demo: producer weight 8 vs
consumer 1 → producer takes the dominant share.

### VM snapshot / restore (`396632d`)
**Checkpoint + rollback.** `VMCTL_SNAPSHOT` captures a guest's full state (GP +
EL1 sysregs + FP + vGIC LRs/MMIO model + vtimer) and its private RAM into one
boot-reserved slot; `VMCTL_RESTORE` rolls it back. **Designed + adversarially
verified first** (workflow returned "needs-fixes"); the load-bearing points:
- `vtimer.cval` is an **absolute** CNTPCT deadline → stored *relative*, rebased
  `now+delta` on restore (else a stale deadline storms the timer IRQ).
- Restore flushes the **target VMID's** stage-2 TLB (temporarily swapping
  `VTTBR_EL2`) — `s2_tlb_flush_all` would flush the caller's VMID. Plus I-cache
  coherence on the rewritten RAM.
- Capture reads only from `vcpu_t` (target is never the current vCPU); RAM sized
  by `ram_size`. Identity + accounting fields never rolled back; dead never
  resurrected; admin-pause never cleared. One fixed 64 MiB slot (bump allocator
  has no free()) → the 8 GiB FermiOS is deliberately not snapshottable.
- Verified live: guest2's beat counter climbs to 0x10, snapshot at 0x0B,
  restore → it jumps back to 0x0C and resumes.

### Live migration / clone (`6bd2d68`)
`VMCTL_MIGRATE` transplants a snapshot into a **different** VM slot, which resumes
in the destination's own stage-2 (different host PA + VMID). Works because guest
state is **host-PA / VMID agnostic** (IPAs + virtual INTIDs only). A new idle
`vmtgt/` VM is the destination; after migrate, two beat streams run (the original
guest2 and its clone). The in-box core of live migration.

---

## In progress (uncommitted) — per-VM fault isolation

Branch `feat/hyp-fault-isolation`. Goal: an unhandled guest trap should **reboot
only the offending VM** (or power it off after a fault budget), not panic the
whole machine — the EL2 analog of FermiOS killing a faulting EL0 task.
`hyp_fatal_trap` now calls `vcpu_fault_isolate` (reuses the in-place
`vcpu_reset`) instead of `hyp_panic`. A `crasher/` guest deliberately faults.

**Status / open issue:** the crasher path works perfectly — rebooted 3× then
powered off, **machine never panics**, the other VMs keep running. But the test
**exposed a latent stage-2 gap**: FermiOS touches a low IPA (~`0x1a34`) that the
stage-2 map leaves invalid, so it now gets fault-isolated instead of reaching its
shell. Previously this would have *panicked the machine* (so fault isolation
surfaced a pre-existing bug). **Not yet committed/pushed** — the next step is to
identify and map that IPA (or confirm it's a guest bug to isolate) before
landing this branch.

---

## Remote branches (each a self-contained, uniquely-named PR)

Pushed to `git@github.com:rituparna-ui/fermi-os.git`:

| Branch | Adds |
|---|---|
| `feat/el2-hypervisor-fermios-guest` | core hypervisor (M1–M5, multi-VM, fair-sched, PSCI, IPC, doorbell, dom0) |
| `feat/hyp-observability` | `VMCTL_STAT` exit accounting |
| `feat/hyp-pv-console` | `HVC_FERMI_LOG` PV console |
| `feat/hyp-weighted-sched` | weighted scheduler + `VMCTL_WEIGHT`/`CPUTIME` |
| `feat/hyp-snapshot-restore` | VM checkpoint / rollback |
| `feat/hyp-live-migration` | live migration (clone) |
| `feat/hyp-fault-isolation` | *(local, WIP — not pushed)* per-VM fault isolation |

The branches stack (each built on the prior); the table lists what each adds.

---

## Hypercall ABI (vendor HVC ids in `x0`)

| HVC id | Name | Meaning |
|---|---|---|
| `0xFE110000` | `HVC_FERMI_YIELD` | yield the rest of the time slice |
| `0xFE110001` | `HVC_FERMI_DOORBELL` | notify the peer VM (inject doorbell IRQ) |
| `0xFE110002` | `HVC_FERMI_VMCTL` | management op (privileged dom0): x1=op, x2=target, x3=arg |
| `0xFE110003` | `HVC_FERMI_LOG` | PV console: x1=buf IPA, x2=len |

`VMCTL` ops (in x1): COUNT, STATE, RUNS, RESET, STOP, START, STAT, WEIGHT,
CPUTIME, SNAPSHOT, RESTORE, MIGRATE.

---

## What runs today

Up to **7 concurrent guests** on one physical CPU: the unmodified **FermiOS**
(8 GiB), a heartbeat guest, an IPC producer + consumer (shared page + doorbell),
a privileged **dom0** control domain, a **migration target**, and a **crasher**
(fault-isolation demo). The hypervisor provides EL2 boot, stage-2 isolation +
controlled sharing, a virtual GICv3, a virtual timer, weighted fair scheduling,
inter-VM IPC + events, a management/observability/PV-console plane, and
checkpoint / restore / live migration.

## Register cheat-sheet (verified values)

| Register | Value | Meaning |
|---|---|---|
| `CPTR_EL2` | `0x32FF` | don't trap guest FP/SIMD/SVE (`0x33FF` traps SVE) |
| `SPSR_EL2` (guest entry) | `0x3C5` | EL1h + DAIF masked |
| `HCR_EL2` (final) | `0x80082039` | RW\|VM\|FMO\|IMO\|AMO\|TWI\|TSC |
| `VTCR_EL2` | `0x80023558` | 40-bit IPA, SL0=1, 4K, PS=40-bit, 8-bit VMID |
| `CNTHCTL_EL2` | `0x1` | allow guest CNTPCT reads, trap CNTP_* |
| `ICC_SRE_EL2` | `0xF` | Enable\|DIB\|DFB\|SRE |
| `ICH_VMCR_EL2` | `0xFF000002` | VPMR=0xFF, VENG1=1, VEOIM=0 |
| stage-2 S2AP | `3<<6` (RW) | NOT stage-1 `PTE_AP_RW` (=0=no-access) |
| HVC/SMC ELR | no advance | only aborts/sysreg/WFx advance ELR_EL2 by 4 |
