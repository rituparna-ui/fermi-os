# FermiOS → EL2 Type-1 Hypervisor: The Full Journey

This document records the complete conversion of the **FermiOS EL1 kernel** into
a **Type-1 (bare-metal) hypervisor at EL2** that boots and interactively runs
unmodified FermiOS instances as **EL1 guest VMs** — what was built, why each
decision was made, the bugs hit and how they were diagnosed, and where things
stand.

- **Branch:** `fermi-hyp2` → pushed to `feat/el2-vhe-hypervisor-fermios-guest`
  on `git@github.com:rituparna-ui/fermi-os.git`
- **Result:** a real unmodified FermiOS runs as a fully interactive EL1 guest
  (its EL0 shell responds to typed `help`/`uptime`/`ps`), and the hypervisor can
  also preemptively round-robin **two** FermiOS guests with full isolation.
- **All code in** `src/hyp/` (~2,600 lines), 18 commits, each verified in QEMU.

---

## 1. Starting point — what FermiOS was

FermiOS is a from-scratch AArch64 (ARMv8-A) kernel for QEMU `virt`:
higher-half kernel (link VA `0xFFFF000040000000`, PA `0x40000000`), 3-level MMU
with TTBR0 (user/identity-low) + TTBR1 (kernel high-half), bitmap PMM, kernel
heap, GICv3, ARM generic timer, a preemptive round-robin scheduler with EL0 user
tasks, SVC syscalls, VFS/FAT32/`/proc`, PCI + virtio (net/blk/rng/console/
balloon), and an interactive EL0 shell.

Critically, it **ran entirely at EL1** with EL0 user tasks — zero EL2/
virtualization awareness. `boot.S` had no `CurrentEL` check; QEMU was launched
as `virt,gic-version=3` (no `virtualization=on`), so `-kernel` entered at EL1.

## 2. The goal and the key architectural decision

**Goal:** turn this EL1 kernel into an EL2 hypervisor hosting **multiple** EL1
guest VMs.

The decisive choice was **how** to sit at EL2. ARMv8 offers two models:

- **Non-VHE EL2:** a classic EL2 with its own single-base stage-1 (no TTBR1, no
  ASIDs). This would force rewriting FermiOS's load-bearing high-half /
  `PHYS_TO_VIRT` / TLBI design — weeks of surgery on proven code.
- **VHE (Virtualization Host Extensions, ARMv8.1):** with `HCR_EL2.E2H=1`, the
  hardware **redirects the host's `_EL1` system-register accessors to the EL2
  bank** (`SCTLR_EL1`→`SCTLR_EL2`, `TTBR0/1_EL1`, `VBAR_EL1`, the `*e1` TLBIs,
  `CPACR_EL1`, etc.). So the *existing* FermiOS MMU/exception/GIC/timer code runs
  **unchanged** as the hypervisor's own EL2 code.

**We chose VHE** (per the user) — minimal new code, the proven kernel stays
intact, and it's exactly the modern KVM model. The same `kernel.elf` serves as
both hypervisor and guest: `boot.S` configures VHE only when entered at EL2; a
reduced-RAM copy of the kernel is embedded as the guest image.

> **Environment gotcha (found immediately):** the dev box's QEMU is **3.1.0**,
> which has **no VHE** (`ID_AA64MMFR1_EL1.VH=0`); writes to `HCR_EL2.E2H` are
> silently dropped. The fix is to build/run inside the `osdev:dev` Docker image
> (QEMU 8.2.2 + `aarch64-linux-gnu-gcc 13.3`), with `-cpu max` and
> `-machine virt,gic-version=3,virtualization=on`. Cortex-A72 (the original
> target) is ARMv8.0 and has no VHE at all.

## 3. How it was built — the design process

Because EL2/virtualization bugs are subtle and can silently corrupt the host,
the design was driven by **parallel multi-agent analysis with adversarial
verification** before writing code:

1. A mapping pass read every EL-relevant subsystem and produced a complete
   inventory of EL1 assumptions.
2. A design pass proposed the VHE world-switch, stage-2, vGIC, vtimer, and guest
   loader.
3. An **adversarial verifier** read the actual on-disk code and refuted unsafe
   claims — it caught, *before they hit code*:
   - that `HCR_EL2.IMO=1` would misroute the **host's own** scheduler tick (the
     EL1 physical timer, PPI 30) to EL2 and hang the box;
   - that `TGE` must be flipped 1→0 on guest entry and restored before any host
     C runs, entirely inside asm with DAIF masked;
   - that the `_EL1` register redirect depends on **E2H, not TGE**;
   - the correct stage-2 start level (SL0=1, 3-level for 40-bit IPA).

Then implementation proceeded **milestone by milestone**, each independently
bootable and verified in QEMU, each its own commit with the verification result
in the message.

---

## 4. The milestones (what & why)

### M1 — Boot the existing kernel at EL2 under VHE *unchanged*
`boot.S` gained a `CurrentEL` preamble: at EL2 it sets `HCR_EL2.{RW,E2H,TGE}`,
`CPTR_EL2.FPEN` (FP), `ICC_SRE_EL2` (before any `ICC_*_EL1`), and `CNTHCTL_EL2`
timer pass-through, then falls through to the *existing* boot path. **Zero C
changes** — the MMU, exceptions, GIC, timer all work via the E2H redirect.
*Why:* prove the cheap VHE win first; the banner reads "Hyper Space" (EL2) and
all MMU self-tests pass at EL2.

### M2 — Dedicated EL2 vector table + trap plumbing
A separate `VBAR_EL2` table (`hyp_vectors.S`) that captures the `_EL2`-banked
syndrome registers (`ELR/SPSR/ESR/HPFAR/FAR_EL2`). Self-tested by firing an
`hvc` from the host and confirming `EC=0x16` capture. *Why:* the guest-exit path
needs `HPFAR_EL2` (stage-2 fault IPA), which the host's normal vectors don't
read.

### M3 — World switch + stage-2 + a trivial EL1 guest
`world_switch.S`'s `vcpu_enter()` (KVM `__guest_enter`/`__guest_exit` style):
saves host context, installs the guest vectors, programs `VTCR_EL2`+`VTTBR_EL2`,
sets guest `HCR_EL2 = E2H|RW|VM`, and `eret`s to EL1 — **returning to the host C
scheduler** on guest trap. `stage2.{c,h}` builds IPA→PA tables (40-bit IPA,
SL0=1 concatenated 1024-entry L1; stage-2 descriptor encoding). A hand-written
EL1 stub runs under stage-2, writes a marker, and HVCs back.
*Why this shape:* a guest must be a schedulable entity that returns control to C,
not an inline-`eret` resume.
*Bug found:* the world switch programmed `VTTBR_EL2` but not `VTCR_EL2` (left at
reset 0) → every stage-2 walk faulted (`EC=0x20`). Adding `VTCR_EL2=0x80023558`
fixed it.

### M4 — EL2-timer time-slicing
A spinning EL1 guest (never yields) preempted by the **EL2 physical timer**
(`CNTHP`, PPI 26) and resumed, counter advancing. *Why CNTHP:* the hypervisor's
preemption source must be independent of the guest's own EL1 timer.

### M5 — vGIC (guest interrupt controller)
`vgic/vgic.{c,h}`: enable `ICC_SRE_EL2` + the GICv3 virtual CPU interface
(`ICH_HCR_EL2.En`, `ICH_VMCR_EL2`), inject via `ICH_LR<n>_EL2`, and trap-emulate
the small GICD/GICR register set FermiOS's `gic.c` touches (the windows are left
stage-2-unmapped so they fault to EL2). *Why:* a full FermiOS guest's
`gic_init()` polls `GICR_WAKER` and writes `GICD_CTLR` — it can't touch the real
GIC. With the virtual interface, the guest's ack/EOI hot path is serviced by the
List Registers in hardware (no per-EOI trap).

### M6 — Guest virtual timer
Enable the guest's EL1 physical-timer PPI (30) on the *real* redistributor so its
`CNTP` IRQ reaches EL2, where we inject vINTID 30. The guest's scheduler ticks.

### M7/M8 — Guest loader + full FermiOS as a guest
A reduced-RAM (128 MiB) FermiOS is built from the same sources
(`-DMEM_SIZE -DGUEST_BUILD`, excluding `src/hyp`), flattened, and embedded via
`guest_blob.S`. The loader PMM-backs the guest IPA window, builds stage-2, copies
the image, and runs it. **A full unmodified FermiOS boots as an EL1 guest** —
its own stage-1 MMU enabling *under* our stage-2 (true nested translation),
through VFS/`/proc`/scheduler to its idle loop.
*Bug found:* the guest NULL-derefed a virtqueue because no virtio device was
passed through; guarded the virtio drivers (and later `virtqueue.c` itself).

### M9 — Multiple guests, preemptive round-robin
The original goal. Two independent FermiOS instances, each with its own VMID,
stage-2, RAM, and vGIC, **preemptively time-sliced** on CNTHP with **full
per-vCPU context save/restore**:
- `vcpu_context.S`: `vcpu_save/restore_el1` via the **`_EL12`/`_EL02` VHE
  aliases** (the guest's EL1 bank, which the host's plain `_EL1` accessors don't
  reach), and `vcpu_save/restore_fp` (q0–q31). Needs `.arch armv8.1-a`.
- Each guest resumes at its own advancing PC across switches — proving the full
  context swap.
*Bug found:* a stray `pc += 4` after HVC (HVC's `ELR_EL2` already points past
the instruction) skipped the loop into garbage; removing it fixed the
round-robin.

### M10 — Per-guest virtual PL011 console
`vuart/vuart.{c,h}`: the guest UART window is left stage-2-unmapped and
trap-emulated per-guest; TX is line-buffered and flushed under a `[vm0]`/`[vm1]`
prefix. *Why:* attributed, non-interleaved console output and tighter isolation
(no guest touches the physical UART).

### M11 — PSCI provider
`psci/psci.{c,h}`: handle guest `hvc` PSCI calls (VERSION/FEATURES/SYSTEM_RESET/
SYSTEM_OFF/CPU_OFF). SYSTEM_RESET **warm-resets** the calling guest (reload image
+ reset vCPU). *Why:* FermiOS's `reboot` command issues PSCI SYSTEM_RESET; the
hypervisor must service it rather than ignore it.

### M12 — Interactive guest console (bidirectional)
The vuart gained an **RX FIFO**; the hypervisor polls the physical UART on each
guest exit and pumps input to the guest, whose `FR.RXFE`/`DR` reads now see it.
First proven with a tiny echo guest (no MMU/PCI) to isolate the input path.

### M13 — A fully interactive *FermiOS* guest
The headline result. Typing `help`/`uptime`/`ps` at the host drives a real
FermiOS guest's EL0 shell, which responds with its command list, timer-driven
uptime, and its own task table. Two key fixes were needed:

1. **The guest-timer interrupt storm.** The guest's EL1 physical timer (PPI 30)
   is *level-triggered*. EOIing it at EL2 before the guest re-armed `CNTP_CVAL`
   re-fired it instantly — and since the guest's handler itself traps (its UART
   access), the guest never finished the handler → **~700,000 IRQs, zero
   progress.** The fix (the standard KVM technique, confirmed via an expert
   analysis): inject the timer through a **hardware-mapped List Register**
   (`ICH_LR.HW=1`, `pINTID=vINTID=30`) and **never physically EOI it**. The
   physical interrupt stays *Active* (parked) throughout the guest's handler —
   including its mid-handler MMIO traps — and the guest's own *virtual* EOI
   deactivates the physical interrupt automatically once it re-arms the timer.
   One IRQ per tick, no storm. (`vgic_inject_hw`)
2. **Fast guest boot.** A full FermiOS scans PCI config space. The guest build
   scans only bus 0 (`MAX_PCI_BUS=1` under `GUEST_BUILD`; QEMU `virt` has one
   bus), and the hypervisor backs the bus-0 ECAM window with all-`0xFF` RAM
   (`stage2_back_ecam`) so config reads return "no device" **without trapping**
   — turning a ~65K-trap scan into a handful of cached reads.

Also hardened `virtqueue.c` (submit/notify/poll no-op on an uninitialised queue)
so a guest with no virtio devices fails cleanly — which benefits the host too.

---

## 5. The final architecture

```
            ┌─────────────────────────────────────────────┐
   EL2      │  FermiOS-as-hypervisor (VHE host, E2H=1)     │
            │  - reuses its own MMU/heap/GIC/timer/UART     │
            │  - src/hyp/: world switch, stage-2, vGIC,     │
            │    vtimer (HW-LR), vUART, PSCI, scheduler     │
            └───────▲───────────────────────────▲──────────┘
                    │ vcpu_enter / trap-exit     │
       ┌────────────┴───────────┐   ┌────────────┴───────────┐
   EL1 │ Guest VM 0 (FermiOS)   │   │ Guest VM 1 (FermiOS)   │
   EL0 │  its own EL0 shell+tasks│   │  its own EL0 shell+tasks│
       │  VMID 1, stage-2 #0     │   │  VMID 2, stage-2 #1     │
       └─────────────────────────┘   └─────────────────────────┘
```

- **Isolation:** each guest has a distinct VMID + stage-2 address space; the
  same guest IPA maps to different host PAs.
- **CPU:** full GP + `ELR/SPSR_EL2` + `SP_EL1` + the guest EL1 sysreg bank (via
  `_EL12`) + FP (q0–q31) are world-switched.
- **Interrupts:** GICv3 virtual interface + `ICH_LR` injection; the timer uses a
  HW-mapped LR so it can't storm.
- **Devices:** per-guest virtual PL011 console (in + out); PCI emulated as "no
  device"; PSCI for power management.
- **Scheduling:** hypervisor preemption via the EL2 physical timer (CNTHP).

## 6. Source map (`src/hyp/`)

| File | Role |
|------|------|
| `hyp.{c,h}` | EL2 detection, the M2 self-test, and the milestone drivers (smoke / time-slice / dual-FermiOS / PSCI test / interactive guest). |
| `world_switch.S` | `vcpu_enter` + guest-exit vectors — the enter/exit core. |
| `hyp_vectors.S` | Dedicated `VBAR_EL2` table for the M2 self-test. |
| `vcpu.h` | `vcpu_t` (asm-visible GP/return block + extended EL1/FP/vGIC/vUART state). |
| `vcpu_context.S` | Guest EL1 bank (`_EL12`) + FP save/restore. |
| `stage2.{c,h}` | Stage-2 IPA→PA tables, `VTCR_EL2`/`VTTBR_EL2`. |
| `vgic/vgic.{c,h}` | Virtual GICv3: virtual interface, LR injection (incl. HW-mapped), GICD/GICR MMIO model. |
| `vuart/vuart.{c,h}` | Per-guest virtual PL011 (TX line-buffer + RX FIFO). |
| `psci/psci.{c,h}` | PSCI 1.1 provider (reset / off). |
| `guest_stub.S` | Hand-written EL1 guests for the demos/self-tests. |
| `guest_blob.S` | `.incbin` of the embedded reduced-RAM FermiOS guest. |

## 7. Build & run

Use the `osdev:dev` Docker image (QEMU 8.2.2 + aarch64 GCC); the bare host
QEMU 3.1.0 lacks VHE.

```sh
# inside the container, from the repo root:
make all && make disk
qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on -m 8G \
  -nographic -cpu max \
  -drive file=build/disk.img,if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel build/kernel.elf
```

- **Default build:** boots an interactive FermiOS guest — type `help`, `ps`,
  `uptime` at its shell.
- **`-DHYP_RUN_DEMOS`:** runs the M3/M4/M9a/M11/M9c self-tests first (all PASS in
  one boot), then the interactive guest.

## 8. Key lessons / non-obvious truths

- **VHE makes a "thin" hypervisor possible** by redirecting `_EL1` accessors to
  the EL2 bank — the rich kernel runs at EL2 nearly unchanged.
- **`_EL12` aliases** are how EL2 touches a *guest's* EL1 bank (needed only for
  multi-guest); they require `.arch armv8.1-a` to assemble.
- **Level-triggered passed-through interrupts need HW-mapped List Registers**,
  or they storm — the single most important correctness lesson here.
- **A guest of a deviceless hypervisor stresses driver robustness** — the virtio
  NULL-queue guards we added are real defensive improvements.
- **The dev environment matters:** the whole project is only testable on a
  VHE-capable QEMU (≥ 4.0); diagnosing the silent E2H drop on 3.1.0 was the first
  real win.

## 9. M14 & M15 (done)

- **M14 — unified multi-guest interactive console (done):** two FermiOS guests
  run *live and preemptively* and you switch console focus between their shells
  with `Ctrl-X`. The multi-guest scheduler became each guest's timer source
  (soft vINTID-30 injection per slice, gated on the guest enabling the timer),
  instead of the shared physical PPI 30, to avoid a cross-guest Active-state
  conflict; guests run with `IMO` so the CNTHP scheduler tick preempts them.
- **M15 — inter-VM shared memory + doorbell (done):** one host page mapped into
  both guests' stage-2 at the same IPA, plus a doorbell hypercall
  (`hvc x0=0xF0000001`) the hypervisor recognises and counts. A producer guest
  writes the shared page + rings; a consumer guest reads it back (verified
  `PC1PC2…PC6`, counter==doorbell_seq).

- **M16 — interrupt-driven inter-VM doorbell (done):** the doorbell hypercall
  now injects a **virtual SPI** (INTID 40) into the peer (consumer) VM's vGIC
  (`vgic_inject_to`), and the consumer takes a real EL1 interrupt — installs a
  vector table, enables the GICv3 CPU interface, WFIs, and its IRQ handler acks
  (`ICC_IAR1_EL1`), reads the shared page, and EOIs. Verified: `PPD2PD3`,
  doorbells=3, consumer IRQ-handler runs=2 → PASS.

## 10. What's next (not yet built)

- A general HVC hypercall ABI, virtio passthrough (real guest disk/net), a
  non-FermiOS guest, dynamic VM lifecycle, and a guest→host security audit.

---

*See `src/hyp/README.md` for the per-file architecture reference, and the git log
on `feat/el2-vhe-hypervisor-fermios-guest` for the milestone-by-milestone commits
with their verification results.*
