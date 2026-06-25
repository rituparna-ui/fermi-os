# Fermi Hypervisor (EL2)

A minimal **type-1 (bare-metal) hypervisor** for AArch64 that runs the existing
FermiOS kernel — unmodified — as an EL1/EL0 guest, and runs several smaller
guests alongside it (5 VMs total) with preemptive round-robin scheduling,
per-VM stage-2 isolation, inter-VM shared memory + an event-channel doorbell,
per-VM PSCI lifecycle, and a privileged dom0-style management plane.

The hypervisor is a separate image from the guest. QEMU loads it via `-kernel`
and enters it at **EL2**; it sets up virtualization and `eret`s down into the
guest(s) at EL1.

```
EL2   Fermi Hypervisor          <-- this directory (src/hyp/)
        stage-2 MMU, vGICv3, vtimer, world switch, trap-and-emulate
EL1   FermiOS guest(s)          <-- src/ (the unmodified kernel)
EL0   guest user tasks
```

## Building & running

Requires QEMU >= ~6 with GICv3 virtualization and a `-cpu max`-class core
(FEAT_VHE, stage-2, CNTVOFF). The repo's Docker image (`osdev:dev`, QEMU 8.2.2 +
gcc 13.3) provides both.

```
make          # builds build/kernel.elf (guest) AND build/hyp.elf (hypervisor)
make run      # -kernel build/hyp.elf on virt,virtualization=on -cpu max -m 9G
```

`make` builds two images: the guest `build/kernel.elf` is objcopy'd to a flat
blob and **embedded inside** `build/hyp.elf` (so QEMU's auto-placed DTB at
`0x40000000` does not collide with a separately-loaded guest). The hypervisor
copies the guest to its load address and `eret`s into it.

## Memory layout (host physical)

QEMU is launched with `-m 9G`, so RAM spans `0x40000000 .. 0x280000000`.

| Region | Host PA | Notes |
|---|---|---|
| VM1 (FermiOS) RAM | `0x40000000 .. 0x240000000` (8 GiB) | the guest's hardcoded `MEM_SIZE` |
| Hypervisor image + pool | `0x250000000 .. ` | text/data/stack + bump allocator |
| VM2 RAM | `0x260000000 .. 0x264000000` (64 MiB) | heartbeat guest |
| IPC producer RAM | `0x264000000` (16 MiB) | + shared page @ `0x266000000` |
| IPC consumer RAM | `0x265000000` (16 MiB) | shares the same page |

The hypervisor links at `0x250000000` — **above** the guest's 8 GiB — so it
never collides with pages the guest's PMM hands out, even with stage-2 off.

## Milestones (git history)

| Commit prefix | What |
|---|---|
| M1 | Boot at EL2, `eret` into unchanged FermiOS at EL1 (no stage-2) |
| M2 | Stage-2 translation (`HCR_EL2.VM=1`), identity IPA→PA |
| M3+M4 | World-switch spine, virtual GICv3, virtual timer |
| M5 | GICD/GICR MMIO trap-and-emulate (guest fully virtualized) |
| multi-VM | Second guest + EL2 round-robin scheduler, per-VM stage-2 isolation |

## Source map

| File | Role |
|---|---|
| `hyp_boot.S` | EL2 reset entry: detect EL2, set SP_EL2/VBAR_EL2/SCTLR_EL2/CPTR_EL2/MDCR_EL2/CNTHCTL_EL2/HCR_EL2, call `hyp_main` |
| `hyp.c` / `hyp.h` | `hyp_main` orchestration, EL2 PL011 console, guest-image placement |
| `hyp_sysregs.h` | Documented EL2 register bit definitions + staged HCR values |
| `hyp_vectors.S` | EL2 vector table + world-switch entry/exit (288-byte GPR-only trap frame) |
| `vm.c` / `vm.h` | ESR_EL2 trap dispatcher: HVC/PSCI, trapped sysreg, WFI, data-abort→MMIO, IRQ |
| `stage2.c` / `stage2.h` | Stage-2 page tables (per-VM L1 roots), VTCR/VTTBR |
| `vgic/vgic.c` / `.h` | Virtual GICv3: CPU-interface enable, LR injection, GICD/GICR MMIO model |
| `timer/vtimer.c` / `.h` | Virtual EL1 physical timer (trap CNTP_*, drive CNTHP, inject vINTID 30) |
| `hyp_gic.c` / `.h` | EL2-side physical GIC bring-up (receive CNTHP PPI 26) |
| `hyp_alloc.c` / `.h` | Hypervisor-private bump allocator (above guest RAM) |
| `vcpu.c` / `.h` / `vcpu_switch.S` | Per-vCPU context + EL2 round-robin scheduler |
| `guest2/` | Tiny standalone EL1 guest (heartbeat printer; self-resets via PSCI) |
| `ipc/` | EL1 guest run by 2 VMs (producer/consumer) for inter-VM shared memory |
| `dom0/` | Privileged EL1 control guest driving the VMCTL management hypercall |
| `*_blob.S` (`guest`, `guest2`, `ipc`, `dom0`) | Embed the flat guest images into the hyp |

## How key subsystems work

### Boot (hyp_boot.S → hyp_main)

`-kernel build/hyp.elf` enters `_hyp_start` at EL2. It checks `CurrentEL == 2`,
sets up the EL2 stack/vectors, programs `CPTR_EL2 = 0x32FF` (do **not** trap
guest FP/SIMD/SVE), `MDCR_EL2` (no debug/PMU traps, `HPMN = PMCR_EL0.N`),
`VPIDR/VMPIDR_EL2`, and calls `hyp_main`, which builds stage-2 + GIC + timer,
creates the vCPUs, and enters the first guest. It never touches `SCR_EL3` — there
is no EL3 on QEMU `virt` without `secure=on`.

### Stage-2 translation

40-bit IPA, 4 KiB granule, SL0=1 → a **concatenated 1024-entry L1** (two 4 KiB
pages, 8 KiB aligned). Stage-2 descriptor encodings differ from stage-1:
`S2AP_RW = 3<<6` (stage-1's `0<<6` means *no access* at stage-2), memory type is
encoded **directly** in `MemAttr[5:2]` (no MAIR), `XN[54:53]` is a 2-bit
enumerated field. Guest RAM → Normal-WB; device windows → Device-nGnRE. GICD/GICR
are left **invalid** so guest MMIO faults to EL2 for emulation.
`VTCR_EL2 = 0x80023558`.

### Virtual timer (the level-triggered trap)

The guest uses the **EL1 physical timer** (`CNTP_*`), which is *level-triggered*:
pure passthrough would re-assert and storm EL2. Instead the hypervisor:

1. Traps guest `CNTP_*` (`CNTHCTL_EL2.EL1PCEN=0`, but allows `CNTPCT` reads).
2. Mirrors the guest's deadline into the **EL2 physical timer** (`CNTHP_*_EL2`).
3. On the `CNTHP` PPI (26) at EL2, injects a **virtual INTID 30** via a vGIC
   List Register and disarms `CNTHP`.
4. The guest services its IRQ and re-arms `CNTP_CVAL_EL0` → traps back → re-arm.

No storm; `timer.c` is unchanged.

### Virtual GICv3

With `HCR_EL2.IMO=1`, the guest's `ICC_*_EL1` CPU-interface accesses are
**hardware-redirected** to the virtual interface — no per-ack/EOI trap. The
hypervisor enables `ICC_SRE_EL2=0xF` + the virtual CPU interface
(`ICH_HCR_EL2.En=1`), seeds `ICH_VMCR_EL2=0xFF000002`, reads the number of List
Registers from `ICH_VTR_EL2` (QEMU reports **4**, not 16 — read it, don't
assume), and injects interrupts by writing a free `ICH_LR<n>_EL2`. GICD/GICR
**MMIO** faults to EL2 and is serviced by a small software model (`GICD_CTLR`,
`GICR_WAKER`, `ISENABLER`, etc.).

### Inter-VM shared memory

Isolation is the default (each VM's IPA `0x40000000` maps to a *different* host
PA), but the hypervisor can also *grant* sharing: it maps one host page into two
VMs' stage-2 spaces at a common IPA (`0x50000000`). The two `ipc` VMs (a
producer and a consumer running the same image, role chosen by `x0`) demonstrate
it — the producer increments a sequence number in the shared page and the
consumer reads back the identical value, while their private RAM stays isolated.
This is the Xen-grant-table / KVM-ivshmem model: same-IPA→same-PA for sharing,
same-IPA→different-PA for isolation. (`s2_build_ipc` in stage2.c.)

**Doorbell (event channel).** The consumer is *event-driven*, not polling: it
sets up its own GICv3 CPU interface + EL1 IRQ vector, enables the doorbell
INTID, and WFIs. The producer, after writing, issues `HVC x0=0xFE110001`
(`HVC_FERMI_DOORBELL`); the hypervisor injects the doorbell vINTID into the
consumer (`vcpu_ring_doorbell` → a live or saved List Register depending on
whether the consumer is current) and marks it runnable. The consumer takes a
virtual IRQ, reads the shared value, and EOIs. A one-word readiness handshake in
the shared page avoids ringing before the consumer's interrupt path is armed.
This is the Xen-event-channel / virtio-notification model.

### Multi-VM world switch

Each guest has a `struct vcpu` with the full context (GPRs, EL1 sysregs, FP,
per-VM vGIC, per-VM vtimer, per-VM stage-2 `VTTBR`). The EL2 scheduler preempts
on the **CNTHP** timer (~10 ms slice). CNTHP is armed to the soonest of {the
scheduler slice, **every** vCPU's vtimer deadline} — so a blocked guest is woken
precisely on its own timer even while another VM runs. On a tick it saves the
outgoing guest's context and restores the incoming guest's. Each guest uses a
**separate stage-2 root**, so they all run at IPA `0x40000000` but map to
different host PA — true memory isolation (except for explicitly shared pages,
see above).

**Fair scheduling (block-on-WFI).** When a guest executes `WFI` (idle, awaiting
its next interrupt) the hypervisor marks its vCPU *blocked* and world-switches
to another runnable VM, instead of letting it busy-trap `WFI` for the rest of
its slice. On each `CNTHP` fire, `vcpu_wake_expired` injects the timer IRQ into —
and marks runnable — any vCPU whose vtimer deadline elapsed, including blocked,
non-current ones (injecting into their *saved* List Registers). The result is
genuine fair time-sharing: an idle, timer-driven guest (FermiOS) and a
compute-bound guest both make steady concurrent progress. A guest may also yield
cooperatively via `HVC x0=0xFE110000`.

> Note: the EL2 *virtual* timer (`CNTHV`/PPI 28) does not deliver IRQs reliably
> on the tested QEMU, so the **physical** EL2 timer (`CNTHP`/PPI 26) drives both
> the vtimer and the scheduler.

## Register cheat-sheet (verified values)

| Register | Value | Meaning |
|---|---|---|
| `CPTR_EL2` | `0x32FF` | RES1 base `0x33FF` with TZ(8)+TFP(10) cleared — don't trap guest FP/SVE (`0x33FF` would trap SVE) |
| `SPSR_EL2` (guest entry) | `0x3C5` | EL1h + DAIF masked |
| `HCR_EL2` (final) | `0x80082039` | RW\|VM\|FMO\|IMO\|AMO\|TWI\|TSC; TGE=0, E2H=0, HCD=0 |
| `VTCR_EL2` | `0x80023558` | T0SZ=24 (40-bit IPA), SL0=1, WBWA, IS, 4 KiB, PS=40-bit, VS=8-bit VMID |
| `VTTBR_EL2` | `(L1 & ~0x1FFF) \| (VMID<<48)` | per-VM stage-2 base, 8 KiB aligned |
| `CNTHCTL_EL2` | `0x1` | EL1PCTEN=1 (allow CNTPCT), EL1PCEN=0 (trap CNTP_*) |
| stage-2 leaf (RAM) | `VALID\|AF\|SH_INNER\|S2AP_RW(3<<6)\|MemAttr Normal-WB(0b1111<<2)` | |
| stage-2 leaf (device) | `VALID\|AF\|S2AP_RW(3<<6)\|MemAttr Device-nGnRE(0b0001<<2)\|XN(0b10<<53)` | |
| `ICC_SRE_EL2` | `0xF` | Enable\|DIB\|DFB\|SRE |
| `ICH_VMCR_EL2` | `0xFF000002` | VPMR=0xFF, VENG1=1, VEOIM=0 |
| `ICH_LR<n>` (inject INTID30) | `State=Pending(1<<62)\|Group1(1<<60)\|prio<<48\|30` | HW=0 (pure virtual) |

Notes captured the hard way: HVC and SMC are **symmetric** — neither advances
`ELR_EL2` (only data/instruction aborts, trapped sysregs, and WFx need `+4`).
The hyp is built `-mgeneral-regs-only` so its GPR-only trap frame can never
clobber the guest's caller-saved q-registers.

### Management plane (dom0 control domain)

One guest (`dom0`) is marked **privileged** and may issue the `VMCTL` management
hypercall (`HVC x0=0xFE110002`, op in x1, target vCPU id in x2) against the
other VMs — the Xen-dom0 / libvirt-`virsh` model. Operations: `COUNT` (how many
VMs), `STATE` (packed runnable/dead/vmid), `RUNS` (schedule count), and the
lifecycle controls `RESET` / `STOP` (pause) / `START` (resume). Non-privileged
VMs get `VMCTL_EPERM`. A paused VM (`vcpu_t.paused`) is skipped by the scheduler
and is *not* auto-resumed by its timer (distinct from a WFI-blocked VM), so STOP
genuinely suspends it until START.

The dom0 guest runs a one-shot script: enumerate all VMs + print their state and
run-count, then pause the heartbeat guest, resume it, and warm-reset the IPC
producer — demonstrating live VM management. (`vcpu_vmctl` in vcpu.c.)

**Exit accounting (xentop-style).** The EL2 dispatcher tallies per-VM exit
counts by reason — HVC, data abort (stage-2/MMIO), trapped sysreg, WFx, and
physical IRQ (`vcpu_stats_t`, bumped in `account_exit`). dom0 reads them via
`VMCTL_STAT` and prints a per-VM table. The profiles fingerprint each VM's
behaviour: FermiOS is sysreg/IRQ-heavy (timer arms + ticks), dom0 is almost
all HVC (management calls), and the event-driven IPC consumer is WFx-heavy with
*zero* IRQ exits — its doorbells are hardware-delivered via List Registers and
never trap to EL2, which is exactly what an exit counter should (not) record.

### VM lifecycle (PSCI)

The hypervisor emulates a per-VM PSCI interface (the guest's `hvc`/`smc`):

- **`PSCI_VERSION`** → reports v1.1.
- **`SYSTEM_RESET`** → *warm-resets only the calling VM*: the hypervisor
  re-copies that VM's pristine image to its host RAM, re-initialises its
  register/FP/vGIC/vtimer state, flushes its stage-2 TLB, and restarts it at its
  entry point. The other VM keeps running — it is a per-VM reset, not a machine
  reset. (Each `vcpu_t` carries an `img_src/img_dst_pa/img_size` triple for the
  reload.)
- **`SYSTEM_OFF`** → powers off the calling VM (marked dead, scheduler skips it);
  if it was the last VM the hypervisor halts.

VM2 demonstrates this: it self-issues `SYSTEM_RESET` every 5 heartbeats and its
banner re-prints each time, while VM1 (FermiOS) runs uninterrupted.

## Known limitations / future work

- **PCI ECAM** is mapped straight-through; no vPCI model.
- Single physical CPU only; no SMP guests.
- The scheduler is round-robin with block-on-WFI; there is no priority or
  weighting between VMs.
