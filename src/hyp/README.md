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
| … dom0 / vmtgt / crasher / hangguest / rng·blk·net·pci clients | `0x268000000 .. 0x276000000` | one 16–64 MiB region each |
| SMP guest RAM | `0x276000000` (16 MiB) | **both** vCPUs share it (one stage-2) |

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
| `smpguest/` | 2-vCPU SMP guest: primary `CPU_ON`s a secondary, they ping-pong an SGI |
| `virtio/virtio_balloon.c` / `balloonclient/` | virtio-mmio memory balloon (inflate/deflate of donated PFNs) + its client |
| `*_blob.S` (`guest`, `guest2`, `ipc`, `dom0`, `smpguest`, `balloonclient`, …) | Embed the flat guest images into the hyp |

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

**Weighted proportional share.** Each vCPU has a `weight` (default 1); its time
slice is `base_slice * weight` (base 10 ms, weight clamped to 16), so a
weight-W VM receives ~W/(Σweights) of the CPU — the Xen-credit / cgroup
`cpu.weight` model. dom0 sets weights live via `VMCTL_WEIGHT` and reads
consumed CPU time via `VMCTL_CPUTIME` (per-VM CNTPCT ticks, accumulated on each
switch-out). The dom0 demo gives the IPC producer 8× the consumer's weight and
shows it accruing the dominant CPU share.

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

### virtio-mmio device (standard paravirtual transport)

Beyond the ad-hoc Fermi hypercalls, the hypervisor emulates a **standard
virtio-mmio entropy (RNG) device** (modern transport, virtio 1.x, Version=2) at a
fixed guest IPA window (`0x0A000000`, left stage-2-invalid so accesses trap, like
the GIC). A guest discovers it via the real virtio register block
(MagicValue/Version/DeviceID), runs the spec handshake
(ACKNOWLEDGE→DRIVER→FEATURES_OK→DRIVER_OK), sets up a **split virtqueue**
(descriptor table + avail ring + used ring) in its own RAM, and kicks via
QueueNotify. The hyp (`virtio/virtio_rng.c`) walks the avail ring, fills each
WRITE descriptor with PRNG bytes, posts a used-ring element, advances `used.idx`,
and injects the device SPI (41). The `rngclient/` guest demonstrates it,
receiving fresh random bytes each request. *Any* virtio-aware guest would drive
this same interface — it is not Fermi-specific.

Design-verified hazards that are load-bearing here:
- **Every guest-supplied IPA** (the three ring bases + each `desc.addr`) is
  bounds-checked via `vcpu_ipa_to_pa` before EL2 dereferences it — this is the
  first interface where a guest hands EL2 bulk pointers, so it's the prime
  VM-escape surface.
- **Cache coherence across the EL2-MMU-off / cacheable-guest boundary**: a new
  `hyp_dcache_inval_range` (`dc civac`) is issued before reading guest-written
  rings; buffers and the used ring are cleaned (`dc cvac`) after writing, with
  `used.idx` published strictly *after* the element + buffer stores (`dsb ish`).
- **16-bit free-running `avail.idx`/`used.idx`** handled with wrapping
  subtraction; ring indexed `% N`; descriptor chains bounded to `N` (DoS guard).

### virtio-mmio block device

A second virtio-mmio device (`virtio/virtio_blk.c`, DeviceID 2) at its own IPA
window (`0x0A001000`), exercising the full virtio request shape the RNG device
does not: a **3-descriptor chain** `[ RO header {type, sector} ][ data ][ WO
1-byte status ]` and **bidirectional** transfer — `VIRTIO_BLK_T_IN` copies the
RAM-backed disk → the guest's (WRITE) buffer, `VIRTIO_BLK_T_OUT` copies the
guest's buffer → disk — plus a device-config region exposing the capacity. The
"disk" is a 32 KiB region carved from the hyp pool (persistent across guest
reboots). The `blkclient/` guest writes an incrementing pattern to sector 1,
reads it back, and verifies the round-trip each iteration. The descriptor
direction is honoured per-descriptor (`F_WRITE` ⇒ device writes the buffer), and
the same cache-coherence discipline (invalidate before read, clean after write)
applies in both transfer directions.

### virtio-mmio network device (loopback NIC)

A third virtio-mmio device (`virtio/virtio_net.c`, DeviceID 1) at `0x0A002000`,
the meatiest: **two virtqueues** — RX (queue 0) and TX (queue 1), routed by
`QueueSel` so each Queue* register write lands in the right per-queue state —
and a 12-byte virtio-net header prepended to every packet, plus a device-config
MAC. The hypervisor is a **loopback NIC**: a frame the guest transmits on TX is
copied into a buffer the guest pre-posted on RX, both buffers are completed, and
the RX interrupt (SPI 43) is injected — so a guest that sends a packet receives
it back. The `netclient/` guest posts an RX buffer, transmits a tagged frame,
and verifies the looped-back tag each iteration. Three virtio devices
(entropy + block + net) now coexist on the shared transport.

### Virtual PCI bus (vPCI / ECAM)

A minimal virtual PCI host bridge (`vpci/vpci.c`) emulated via stage-2 traps on
a small ECAM window (`0x0A003000`, covering bus 0 / slot 0 / func 0). A guest
enumerates the bus the standard way and configures the one device it finds —
proving config-space access and BAR sizing, not just fixed MMIO. The emulated
endpoint is a "fermi demo" device (vendor `0x1234`, device `0xBEEF`) with a
single 64 KiB 32-bit memory BAR. The classic **BAR-sizing probe** works: the
guest writes `0xFFFFFFFF` to BAR0 and reads back the size mask
(`~(size-1) | type`), then programs a real base which the device latches; the
Command register enables Memory Space + Bus Master. Config reads honour
byte/halfword/word widths via sub-word masking. The `pciclient/` guest runs the
full flow (scan → size → assign → enable) and prints the result. This is the
PCI-discovery counterpart to the fixed-window virtio-mmio devices.

#### MSI-X (message-signaled interrupts)

The vPCI device also exposes an **MSI-X capability** (cap ID `0x11`), the modern
PCIe interrupt model: instead of a wired INTx line, the device "signals" by a
memory write that a GICv3 turns into an INTID. The capability advertises a
2-vector table + PBA living in **BAR1** — a *fixed-base* memory BAR pinned to a
dedicated trapping window (`0x0A005000`), so guest reads/writes of the table trap
to EL2 and the authoritative table/PBA stay in an EL2-local struct (a guest-chosen
BAR base couldn't be left stage-2-invalid, so BAR1 reports its size to the sizing
probe but ignores base writes). Each 16-byte table entry is `{Msg Addr Lo, Hi,
Msg Data, Vector Control}`; the driver finds the cap by walking the capability
list, programs entries, unmasks, and sets MSI-X Enable.

Two honest deviations from real MSI-X (no ITS model, single PE): (1) a real
device signals by writing `Msg Data` to `Msg Addr` (a GIC doorbell PA) — here the
guest writes a **doorbell register** (window `+0xC00`) naming the vector to fire,
and the hyp injects the vINTID directly; the programmed `Msg Addr` is *recorded
but never dereferenced*. (2) `Msg Data` is programmed as the SPI INTID directly
(45/46), **clamped** to the device's own SPI range so a guest can never inject a
foreign or hypervisor INTID — and `vgic_inject_spi_try` independently rejects
anything outside the SPI range `32..1019` (defense-in-depth; kept separate from
`vgic_inject_ppi` so the vtimer's PPI 30 path is unaffected).

The **mask / PBA / deferred-delivery state machine** is spec-faithful: ringing a
*masked* (or function-masked, or MSI-X-disabled) vector sets its **Pending Bit
Array** bit and injects nothing; on any delivery-enabling edge (per-vector unmask,
Function Mask clear, or MSI-X Enable) a single PBA consumer delivers the pending
vector and clears its PBA bit **only on confirmed List-Register enqueue** (a
full-LR condition leaves it pending — no lost interrupt). The `pciclient/` guest
demonstrates all three: immediate delivery (ring unmasked vector 0 → INTID 45),
the mask path (ring masked vector 1 → `PBA=0x2`, no IRQ), and deferred delivery
(unmask vector 1 → INTID 46 fires, `PBA=0x0`).

### virtio-mmio memory-balloon device

A fourth virtio-mmio device (`virtio/virtio_balloon.c`, DeviceID 5) at
`0x0A004000`, demonstrating cooperative guest↔hypervisor memory management. It
has **two virtqueues** — inflateq (queue 0) and deflateq (queue 1) — and a
device-config region exposing `num_pages` (the target balloon size, device-owned
/ read-only to the guest) and `actual` (pages currently ballooned, driver-owned
/ read-write). The driver puts **arrays of little-endian u32 PFNs** (4 KiB page
frame numbers) on a queue; unlike the rng/blk/net buffers these are
**device-READ** (no `DESC_F_WRITE`), the opposite direction. The device
self-drives: on each write-side trap it runs a `CNTPCT` clock that retargets
`num_pages` between an inflate goal (4 MiB) and 0 and raises a **config-change
interrupt** (InterruptStatus bit 1, distinct from the used-buffer bit 0), bumping
**ConfigGeneration** so the driver's `read-gen / read-num_pages / re-read-gen`
snapshot is consistent — and the autopilot fires *only* on write traps, never on
a config read, so the snapshot can never tear. The `balloonclient/` guest polls
the target and inflates/deflates the difference, walking `balloon=` up to the
goal and back to 0 forever, exercising **both** paths with no external trigger.

The honest deviation (load-bearing): this hypervisor's stage-2 is a **fixed
linear map built once at boot — there is no runtime unmap**, so a balloon cannot
return pages to the host. "Inflation" therefore **zeroes** each donated page
(proving the hyp legitimately reuses its contents) and counts it; the page stays
mapped. Every inflate line prints `NOT host-unmapped; fixed stage-2` so the demo
never overclaims. Everything else is virtio-1.x faithful. Security is taken
seriously even for this toy: every donated PFN is bounds-checked via
`vcpu_ipa_to_pa` before the hyp zeroes it (a PFN aimed at the MMIO window,
another VM, or the hyp yields `pa==0` and is skipped — VM-escape defense), the
PFN array is snapshotted whole before any zeroing (TOCTOU closed), and a hard
per-notify page budget caps total zeroing work so a hostile driver cannot stall
EL2. Four virtio devices (entropy + block + net + balloon) now coexist on the
shared transport.

### Paravirtualized console (PV log)

A guest can emit a log line through the hypervisor instead of poking the raw
UART: `HVC x0=0xFE110003` with a buffer IPA in x1 and length in x2
(`HVC_FERMI_LOG`). The hypervisor translates the guest IPA to a host PA —
**bounds-checked** against that VM's private RAM window (`vcpu_ipa_to_pa`), so a
malicious/buggy guest cannot make the hyp read arbitrary host memory — prints
the bytes tagged with the VM name (`[guest2] ...`), and returns the count. This
is the virtio-console / Xen-console paravirtualised-device pattern: the host
multiplexes and attributes guest output. VM2's heartbeats use it.

### Per-VM fault isolation

An unhandled trap from a guest (a stage-2 fault on an unmapped IPA, an
unrecognised sync exception, etc.) no longer panics the whole machine. Instead
`hyp_fatal_trap` reports it and calls `vcpu_fault_isolate`, which **reboots just
the offending VM** (warm-reset in place) — or, once it exceeds `VCPU_FAULT_MAX`
reboots, **powers it off** (so a guest that faults immediately on every restart
can't spin the hypervisor). Every other VM keeps running. This is the EL2 analog
of FermiOS killing a faulting EL0 task. Traps from EL2 itself are real
hypervisor bugs and still panic. The `crasher/` guest demonstrates it: it
dereferences an unmapped IPA, gets rebooted 3× then powered off, while FermiOS,
dom0, and the IPC pair run on undisturbed.

> Subtlety worth recording: once `hyp_fatal_trap` *returns* (it used to be
> `noreturn`/panic), the data-abort handler must `return` immediately after it —
> otherwise it falls through and keeps emulating against the *rebooted* VM's
> freshly-restored trap frame, corrupting it.

### Liveness watchdog

Fault isolation catches a guest that *crashes*; the watchdog catches one that
*hangs* (livelocks without faulting). A guest arms a watchdog with
`HVC_FERMI_WDOG` (x1 = timeout ticks) and must "pet" it (call again) before the
deadline. On every scheduler tick the hypervisor checks all VMs
(`vcpu_check_watchdogs`); any whose deadline has passed is **rebooted** (and its
watchdog disarmed — the fresh guest re-arms if it wants). The `hangguest/` VM
demonstrates it: it pets 3×, then spins forever in a tight loop with no pets or
traps, and the hypervisor reboots it on the missed deadline — repeatedly — while
every other VM (including the crasher's fault-isolation cycle) runs on. Together
with fault isolation this gives the EL2 analog of OS process supervision:
recovery from both crashes and hangs, per VM, without panicking the machine.

### VM snapshot / restore (checkpoint + rollback)

dom0 can checkpoint a guest's *complete* state and roll it back later
(`VMCTL_SNAPSHOT` / `VMCTL_RESTORE`) — the foundation of live migration and
fault recovery. The snapshot captures the target's full execution state (GP +
EL1 sysregs + FP + vGIC LRs/MMIO model + vtimer) and its private RAM into one
boot-reserved slot, then restore rolls it all back. Verified live: VM2's beat
counter climbs to 0x10, dom0 snapshots at 0x0B, and after `VMCTL_RESTORE` the
guest jumps back to 0x0C and resumes — a precise rollback.

The design was adversarially reviewed; the load-bearing correctness points:
- **vtimer.cval is an absolute CNTPCT deadline** — stored *relative* and rebased
  to `now + delta` on restore, else a stale deadline storms the timer IRQ.
- **Restore flushes the TARGET VMID's** stage-2 TLB (temporarily swapping
  `VTTBR_EL2`), not the caller's — and makes the rewritten RAM I-cache coherent.
- Capture reads from the target's `vcpu_t` (it is never the current vCPU), never
  from hardware; RAM is sized by `ram_size` (not the smaller boot blob).
- Identity + accounting fields (vmid, vttbr, img_*, run_count, cpu_ticks, stats,
  weight) are **never** rolled back; a dead VM is never resurrected and an
  admin-paused VM is never silently un-paused.
- One **boot-reserved fixed slot** (the bump allocator has no free()), capped at
  64 MiB — so the 8 GiB FermiOS is deliberately not snapshottable (rejected with
  `VMCTL_EINVAL`). A `valid` + id/vmid/ram_size stamp guards against restoring a
  stale or mismatched snapshot over a live guest. (`src/hyp/snapshot.c`.)

### Live migration (clone)

`VMCTL_MIGRATE` transplants a snapshot into a *different* VM slot: the captured
guest resumes executing in the destination's own stage-2 (different host PA +
VMID). This works because guest execution state is **host-PA / VMID agnostic** —
it references IPAs and *virtual* INTIDs, never host physical addresses — so the
same state runs unchanged under a different container. The demo migrates the
heartbeat guest into an idle "migration target" VM, which then resumes printing
beats from the migrated counter (two beat streams now run, the original and its
clone, each on its own RAM). Shares `apply_snapshot_to()` with restore; the only
difference is the precondition (clone requires a *matching ram_size* but a
*different* id, vs restore's exact id/vmid match). This is the in-box core of
live migration — a cross-machine version would add a transfer of the same
captured blob.

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

### SMP guests (multi-vCPU VMs)

A VM can have more than one vCPU. Sibling vCPUs of one SMP VM share a single
stage-2 (the same `VTTBR_EL2` / `VMID`, so an IPA maps to the same host PA for
all of them — a real shared-memory SMP model) and a `group_id`, but each carries
a distinct `MPIDR`/affinity. The siblings are still time-sliced onto the one
physical CPU by the EL2 scheduler; "SMP" here means the *guest* sees multiple
CPUs, not that the host has them.

- **Per-vCPU affinity.** `MPIDR_EL1` is virtualised by `VMPIDR_EL2`, which is
  reloaded on **every** world switch (`vcpu_load`) — otherwise siblings would all
  read the same affinity. The primary is `0x80000000` (Aff0=0), the secondary
  `0x80000001` (Aff0=1); bit 31 is RES1, U=0.
- **`PSCI CPU_ON` (SMC64 `0xC4000003`).** `x1`=target affinity, `x2`=entry IPA,
  `x3`=context_id. The hyp finds the in-group sibling whose affinity matches
  (masking the U/MT/RES1 flag bits), brings it up at `entry` with `x0`=context_id
  in EL1h, and marks it `online`. Returns `SUCCESS` / `ALREADY_ON` /
  `INVALID_PARAMETERS`. A secondary starts `online=0` and is **never scheduled**
  until `CPU_ON` (enforced in `pick_next` and `vcpu_wake_expired`).
  `AFFINITY_INFO` (`0xC4000004`) reports ON/OFF.
- **`PSCI CPU_OFF` (`0x84000002`) — CPU hotplug.** A secondary can power *itself*
  down: the hyp sets its `online=0` (but **not** `dead` — it stays resurrectable)
  and switches away, exactly like `SYSTEM_OFF` but reversible. A later `CPU_ON`
  re-onlines it (re-seeding its vGIC/vtimer), giving the full online→offline→
  online hotplug cycle. Restricted to SMP secondaries (`Aff0 != 0`); a primary or
  single-vCPU VM is `DENIED` and must use `SYSTEM_OFF` (so a VM can never strand
  itself with no online sibling to `CPU_ON` it back, matching Linux never
  offlining CPU0). One control-flow subtlety: PSCI `CPU_OFF` returns *nothing* on
  success, so once `vcpu_psci_cpu_off` has switched away, the trap frame belongs
  to the **next** vCPU — `handle_psci` must **not** write `x0` then (it would
  clobber the next vCPU's register); only the `DENIED` failure path returns a
  value. Every vCPU-iterating site (`pick_next`, `vcpu_wake_expired`,
  `hyp_cnthp_arm`, `vcpu_check_watchdogs`) gates on `online`; the
  `hyp_cnthp_arm` gate is load-bearing — without it an offlined secondary's
  stale-armed vtimer would pull `CNTHP` to a past deadline in a tight re-fire
  livelock.
- **Inter-processor SGIs.** The HW virtual interface only delivers to the
  *resident* vCPU, so a guest `ICC_SGI1R_EL1` write that targets a sibling must
  be routed in software. We enable `ICH_HCR_EL2.TC` (Trap Common) **per-vCPU, for
  SMP VMs only**; the trap decodes the SGI's `TargetList`/affinity/`IRM` and
  injects the SGI INTID into each matching sibling (live List Register if it is
  current, saved `lr[]` otherwise — waking blocked ones). Single-vCPU VMs leave
  TC off, so their CPU-interface accesses still flow straight to hardware.
- **The `ICC_PMR_EL1` caveat.** `TC` traps the *whole* common ICC register group,
  not just `ICC_SGI1R_EL1` — including `ICC_PMR_EL1`. So for SMP vCPUs the hyp
  also emulates `ICC_PMR_EL1`, forwarding it to `ICH_VMCR_EL2.VPMR`. (This is why
  TC is scoped to SMP VMs: enabling it globally would fault every guest that
  programs its priority mask during GIC bring-up.)

`smpguest` demonstrates the whole path: the primary `CPU_ON`s the secondary, then
the two ping-pong an SGI (the hyp routing it across the time-sliced siblings each
bounce) while incrementing a counter in their shared RAM. After a few pings the
secondary `CPU_OFF`s itself; the primary polls `AFFINITY_INFO`, sees it go OFF,
counts a hotplug cycle, `CPU_ON`s it again, and the cycle repeats forever — a
self-sustaining online→offline→online hotplug loop.

## Known limitations / future work

- One physical CPU: guest vCPUs (including SMP siblings) are time-sliced, not
  run in parallel.
- The scheduler is weighted round-robin with block-on-WFI; there is no priority
  inheritance or gang-scheduling of an SMP VM's vCPUs.
