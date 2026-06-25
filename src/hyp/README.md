# Fermi Hypervisor (EL2)

A minimal **type-1 (bare-metal) hypervisor** for AArch64 that runs the existing
FermiOS kernel — unmodified — as an EL1/EL0 guest, and can run a second guest
alongside it with preemptive round-robin scheduling.

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
| VM2 RAM | `0x260000000 .. 0x264000000` (64 MiB) | tiny second guest |
| Hypervisor image + pool | `0x250000000 .. ` | text/data/stack + bump allocator |

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
| `guest2/` | Tiny standalone EL1 second guest (heartbeat printer) |
| `guest_blob.S`, `guest2_blob.S` | Embed the flat guest images into the hyp |

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

### Multi-VM world switch

Each guest has a `struct vcpu` with the full context (GPRs, EL1 sysregs, FP,
per-VM vGIC, per-VM vtimer, per-VM stage-2 `VTTBR`). The EL2 scheduler preempts
on the **CNTHP** timer (~50 ms slice), multiplexed with the running guest's
vtimer via `min(deadline)`. On a tick it saves the outgoing guest's context and
restores the incoming guest's. A guest can also yield cooperatively via
`HVC x0=0xFE110000`. The two guests use **separate stage-2 roots**, so both run
at IPA `0x40000000` but map to different host PA — true memory isolation.

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

## Known limitations / future work

- **Scheduler fairness** is coarse: a spin-heavy guest under-serves a
  timer-driven one. Needs slice tuning and per-VM timer accounting across
  switches.
- **PSCI SYSTEM_RESET** halts rather than warm-resetting.
- **PCI ECAM** is mapped straight-through; no vPCI model.
- Single physical CPU only; no SMP guests.
