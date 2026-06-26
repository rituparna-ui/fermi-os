# FermiOS EL2 Hypervisor (`src/hyp/`)

This directory turns FermiOS into a **Type-1 (bare-metal) hypervisor** that runs
at **EL2** using the ARMv8.1 **Virtualization Host Extensions (VHE)**, hosting
one or more unmodified FermiOS instances as **EL1 guest VMs**.

The same `kernel.elf` is *both* the hypervisor and the guest: `boot.S` detects
the exception level it is entered at and configures VHE only when it lands at
EL2. A reduced-RAM copy of the kernel is built as the guest image and embedded
in the hypervisor (see `guest_blob.S`).

> Requires a VHE-capable core and QEMU ≥ 4.0. Build/run in the `osdev:dev`
> Docker image (QEMU 8.2.2, `aarch64-linux-gnu-gcc`); the older host QEMU 3.1.0
> lacks VHE. See the repo root and the project notes for the exact commands.

---

## The VHE idea (why this is small)

Under `HCR_EL2.E2H=1`, when the hypervisor executes at EL2 the hardware
**redirects its `_EL1` system-register accessors to the EL2 bank**
(`SCTLR_EL1`→`SCTLR_EL2`, `TTBR0/1_EL1`, `VBAR_EL1`, the `*e1` TLBIs, etc.).
So the *existing* FermiOS MMU/exception/GIC/timer code runs **unchanged** as the
hypervisor's own EL2 code — only a small boot preamble and the guest-facing
machinery below are new.

To touch a **guest's** EL1 state from EL2 (needed only for multi-guest context
switches), the `_EL12` / `_EL02` register aliases are used instead
(`vcpu_context.S`).

---

## World switch

`world_switch.S` implements a KVM-style `vcpu_enter(vcpu_t*)` that **returns to
the host C scheduler** when the guest traps (not an inline `eret` resume):

1. save host callee-saved regs + `VBAR_EL2`/`HCR_EL2`/`DAIF` on the EL2 stack,
   mask `DAIF`;
2. install the guest-exit vector table in `VBAR_EL2`;
3. program `VTCR_EL2` + the guest's `VTTBR_EL2` (stage-2 base + VMID), flush
   stage-1&2 TLB;
4. set guest `HCR_EL2` = `E2H | RW | VM` (+ optional per-vCPU `hcr_extra`, e.g.
   `IMO` to route physical IRQs to EL2). **`TGE` is cleared for the guest** and
   restored to 1 before any host C runs again;
5. restore guest GPRs and `eret` to EL1.

On a guest trap the exit path saves the guest GPRs + `ELR/SPSR/ESR/FAR/HPFAR_EL2`
into the `vcpu_t`, restores the host `HCR_EL2`/`VBAR_EL2`/`SP`/`DAIF`, and `ret`s
back into `vcpu_enter`'s caller.

`HCR_EL2.IMO` is deliberately **not** set unless a vCPU opts in: the host's own
scheduler tick is the EL1 physical timer (PPI 30), and routing it to EL2 mid-guest
would misdeliver it. The hypervisor's own preemption uses the **EL2** physical
timer `CNTHP` (PPI 26) instead.

---

## Files

| File | Role |
|------|------|
| `hyp.h` / `hyp.c` | `vcpu_t`-agnostic glue: EL2 detection, the M2 self-test dispatcher, and the milestone drivers (smoke guest, time-slice demo, dual-FermiOS scheduler, PSCI test, interactive guest). |
| `hyp_vectors.S` | A dedicated `VBAR_EL2` table + `hyp_exception_common` (used by the M2 self-test; reads the `_EL2`-banked syndrome registers incl. `HPFAR_EL2`). |
| `world_switch.S` | `vcpu_enter` + the guest-exit vector table / `guest_exit_common`. The core enter/exit. |
| `vcpu.h` | `vcpu_t` (GP + `ELR/SPSR_EL2` + `SP_EL1` + `VTTBR` with fixed asm offsets, then the extended per-vCPU EL1/FP/vGIC/vUART state). |
| `vcpu_context.S` | `vcpu_save/restore_el1` (guest EL1 bank via `_EL12`/`_EL02`) and `vcpu_save/restore_fp` (q0–q31). Needs `.arch armv8.1-a`. |
| `stage2.{c,h}` | Stage-2 (IPA→PA) page tables for the MMU-on host: 40-bit IPA, 4 KiB granule, SL0=1 concatenated 1024-entry L1; stage-2 descriptor encoding (`S2AP`, inline `MemAttr`, `XN`); `VTCR_EL2`/`VTTBR_EL2`. |
| `vgic/vgic.{c,h}` | Virtual GICv3: enable `ICC_SRE_EL2` + the virtual CPU interface, inject via `ICH_LR<n>_EL2`, and a GICD/GICR MMIO software model (the windows are left stage-2-unmapped so they trap). Per-vCPU save/restore. |
| `vuart/vuart.{c,h}` | Per-guest virtual PL011 console: trap-and-emulate `DR`/`FR`/init regs, line-buffered TX with a `[name]` prefix and an RX FIFO fed from the host console. |
| `psci/psci.{c,h}` | Minimal PSCI 1.1 provider (VERSION/FEATURES/SYSTEM_RESET/SYSTEM_OFF/CPU_OFF); SYSTEM_RESET warm-resets the calling guest. |
| `hvc/hvc.{c,h}` | Unified HVC hypercall ABI: PSCI + vendor services (VERSION/PUTC/VM_INFO/YIELD/DOORBELL/BLK/NET) folded into one dispatcher returning an action enum. |
| `fdt.{c,h}` | Minimal flattened-device-tree (DTB) builder for foreign guests (M22). |
| `miniguest/` | A standalone, non-FermiOS AArch64 EL1 guest (own start.S/main.c/linker.ld) that boots via `x0=DTB` and parses the device tree. |
| `guest_stub.S` | Small hand-written EL1 guests used by the demos/self-tests (marker write + HVC, spin, heartbeat, PSCI, interactive echo). |
| `guest_blob.S` | `.incbin` of the reduced-RAM FermiOS guest image (`build/guest.bin`) into the hypervisor. |

---

## Milestones (each verified in QEMU 8.2.2)

| # | What |
|---|------|
| M1 | Boot the existing kernel at EL2 as the VHE host (banner reads "Hyper Space"). |
| M2 | Dedicated `VBAR_EL2` table + trap plumbing; HVC self-test. |
| M3 | World switch + stage-2 + a trivial EL1 guest that runs and HVCs back. |
| M4 | Time-slice a spinning EL1 guest off the EL2 physical timer (CNTHP). |
| M5 | vGIC — a full FermiOS guest completes `gic_init`. |
| M6 | Guest virtual timer — the guest's scheduler ticks. |
| M7/M8 | Guest loader; a full unmodified FermiOS boots as an EL1 guest to its scheduler. |
| M9 | Multi-guest round-robin: two full FermiOS guests preemptively time-sliced with full per-vCPU save/restore. |
| M10 | Per-guest virtual PL011 (attributed `[vm0]`/`[vm1]` consoles). |
| M11 | PSCI provider (guest warm reboot / power-off). |
| M12 | Interactive guest console (host input routed to a guest, bidirectional). |
| M13 | A real unmodified FermiOS runs as a **fully interactive** EL1 guest — its EL0 shell responds to typed `help`/`uptime`/`ps`. |
| M14 | **Two** FermiOS guests run preemptively **and** interactively at once; `Ctrl-X` switches console focus between their shells. |
| M15 | Inter-VM shared memory + a doorbell hypercall (a para-virt primitive). |
| M16 | Interrupt-driven doorbell: the hypercall injects a **virtual SPI** into the peer VM, whose EL1 IRQ handler services it. |
| M17 | Unified **HVC hypercall ABI** (`hvc/`): SMCCC-style numbered services (VERSION/PUTC/VM_INFO/YIELD/DOORBELL) with PSCI folded in. |
| M18 | **Guest→host security audit** (adversarially verified): fixed a guest-reachable vGIC NULL-deref host-DoS + an LR-count OOB; the rest of the trap surface verified sound. |
| M19 | **Paravirt block device:** a guest reads the real host disk via block hypercalls, with its buffer IPA safely stage-2-translated (DMA-equivalent). |
| M20 | **Paravirt network device:** a guest does a real NIC round-trip (MAC query + ARP to the gateway + RX of the reply) via net hypercalls. |
| M21 | **Dynamic VM lifecycle:** create/run/destroy a VM at runtime with leak-free stage-2 teardown (free-page count returns to baseline across cycles). |
| M22 | **Foreign (non-FermiOS) guest:** boot a standalone AArch64 guest via the standard `x0=DTB` boot protocol; it parses the hypervisor-built device tree to discover its UART + RAM. |
| M23 | **SMP guest:** two vCPUs in one VM (shared stage-2, distinct VMPIDR); the boot vCPU brings up the secondary via PSCI `CPU_ON`. |
| M24 | **OS-grade device tree:** the DTB now carries `/chosen`, `/psci`, `/cpus`, `/timer` and a GICv3 `/intc` — the boot contract a real AArch64 OS reads. |
| M25 | **vCPU fault isolation:** a misbehaving guest's illegal access is decoded and reaps only that VM; the host + sibling VMs keep running. |
| M27 | **Per-VM observability:** every guest exit is accounted by class (hvc/mmio/irq/fault) for a virsh-style introspection summary. |
| M28 | **Real Linux guest:** a full mainline Linux 6.6 boots as an EL1 guest via `x0=DTB`; it parses our device tree (memory/cmdline/cpus/PSCI/GIC). |
| M29 | **Fuller vGIC → Linux boots to init:** with a real GICD/GICR register model, Linux brings up its GICv3 + arch timer, switches clocksource, calibrates BogoMIPS, and execs `/sbin/init` (panics only for lack of a rootfs). |

The default hypervisor build runs **two** interactive FermiOS guests (M14);
`Ctrl-X` cycles console focus. Build with `-DHYP_RUN_DEMOS` to run the
M3/M4/M9a/M11/M15/M16/M17/M19/M20/M21/M22/M23/M24/M25/M9c self-tests first.

### Regression run

`src/hyp/run-demos.sh` builds the `-DHYP_RUN_DEMOS` image, boots the whole
suite in one QEMU run, and asserts every milestone's PASS marker — a single
end-to-end regression check:

```sh
docker run --rm -v "$PWD":/work -w /work osdev:dev bash src/hyp/run-demos.sh
# -> "ALL MILESTONES PASS (M1-M25)"
```

### M14: the hypervisor as the guest timer source

Two guests can't share the single physical EL1 timer (PPI 30) via the M13
HW-mapped-LR trick — the shared physical Active state would block the peer. So
in M14 the **hypervisor itself is each guest's timer**: it soft-injects vINTID
30 (a `HW=0` LR) once per scheduler slice, gated on the guest having enabled the
timer PPI (`vgic_intid_enabled(30)`, i.e. past `gic_init`). Guests run with
`IMO` so the EL2 scheduler tick (CNTHP, PPI 26) preempts them even in `WFI`.

### The guest-timer interrupt: HW-mapped List Registers

The guest's EL1 physical timer (PPI 30) is **level-triggered**. If the
hypervisor EOIs it at EL2 before the guest re-arms `CNTP_CVAL`, it re-fires
immediately and the guest never finishes its handler (whose own MMIO traps back
to EL2) — an interrupt storm. The fix (standard KVM technique) is to inject the
timer through a **hardware-mapped List Register** (`ICH_LR<n>_EL2.HW=1`,
`pINTID=vINTID=30`) and **never physically EOI it**: the physical interrupt
stays *Active* (parked) throughout the guest's handler, and the guest's own
virtual EOI deactivates the physical interrupt automatically once it re-arms the
timer. See `vgic_inject_hw()`.

### Fast guest boot (PCI)

A full FermiOS guest scans PCI config space. The guest build limits the scan to
bus 0 (`MAX_PCI_BUS=1` under `GUEST_BUILD`; QEMU `virt` has one bus), and the
hypervisor backs the bus-0 ECAM window with all-`0xFF` RAM (`stage2_back_ecam`)
so config reads return "no device" without trapping.

## Known limitations / future work

- The vGIC (post-M29) emulates enough GICD/GICR for Linux's GICv3 driver to
  come up, but it is still not a complete GICv3 (no SPI routing to guests, no
  ITS/LPIs, single redistributor). A FermiOS or Linux guest boots and takes its
  timer IRQ; richer interrupt topologies are future work.
- Real Linux boots to the point of exec'ing init (M29) but has **no rootfs**
  (kernel built with no initramfs), so it panics with "No working init found" —
  the correct end of a kernel-only boot. Adding an initramfs would reach a
  shell.
- Guest *virtio* is not passed through (PCI config space is emulated as "no
  device"); real device access is via paravirt hypercalls instead (M19 block,
  M20 net), which translate guest buffer IPAs safely.
- The world switch shares one EL2 stack frame, so the serial scheduler is not
  reentrant across nested guest exits (fine as used).
