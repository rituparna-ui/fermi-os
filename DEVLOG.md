# Fermi OS → Type-1 Hypervisor → Linux Guest: Development Log

This document records the full journey of turning **Fermi OS** (a bare-metal
aarch64 kernel) into a **Type-1 (bare-metal) hypervisor** that boots an
unmodified **Linux** kernel to a userspace shell, alongside Fermi itself, as
mutually-isolated guests — and then gives that Linux guest emulated **virtio**
devices. It captures *what* we built at each step and, importantly, *why*, plus
the bugs we hit and how we found them.

For the steady-state architecture, see [`HYPERVISOR.md`](HYPERVISOR.md). This
file is the narrative/decision log.

---

## 0. The goal and the constraints

**Goal (stated by the user):** "the ultimate goal is to be able to run a linux
kernel as guest on the hypervisor I build."

**Starting point:** Fermi OS — a from-scratch ARMv8-A kernel (PL011 UART, PMM,
MMU with higher-half kernel, GICv3, generic timer, preemptive scheduler, EL0
user tasks, syscalls, VFS, FAT32, PCI/virtio drivers, an interactive EL0
shell). It ran at **EL1** under QEMU `virt`.

**Hard constraint that shaped everything:** the target is `-cpu cortex-a72`,
which is **ARMv8.0-A — no VHE** (Virtualization Host Extensions). So we could
*not* simply relocate Fermi's EL1 code to EL2 (no `TTBR1_EL2`, no transparent
`_EL1`→`_EL2` redirection). The clean design was therefore a **separate, thin
EL2 hypervisor** that runs Fermi *unmodified at EL1 as a guest* — the user
explicitly chose this ("Option B").

**Build/run environment:** everything is built and tested inside a Docker
container named `osdev` (the aarch64 cross-toolchain + QEMU live there; the
project is mounted at `/mnt/fermi`). The host `/home/rituu/fermi-os` is the same
tree. QEMU drops into an interactive shell, so test runs use a timeout and pipe
input.

**Git workflow (user-directed):** never open PRs, never touch `main`; commit and
push every milestone to **uniquely-named branches** on
`git@github.com:rituparna-ui/fermi-os.git` (SSH, set up mid-project).

---

## 1. Milestone-by-milestone

Each milestone was built incrementally and **verified in QEMU before commit**.

### M1 — EL2 bring-up + stage-2 + Fermi as a guest
*Why:* establish the EL2 layer beneath Fermi without disturbing it.
- `Makefile`: QEMU machine → `virt,gic-version=3,virtualization=on` (this is what
  exposes EL2 *and* the virtual GIC).
- `boot.S`: detect entry at EL2 via `CurrentEL`; if EL2, run `hyp_init`, then
  `eret` to EL1 to continue the *existing* Fermi boot path (so Fermi runs
  unmodified). Falls through unchanged if entered at EL1.
- `hyp.c`/`hyp.h`/`vector_el2.S`: a flat **stage-2 identity map** (IPA==PA, 1 GiB
  blocks, RAM=Normal/MMIO=Device) via `VTTBR_EL2`/`VTCR_EL2`; `CNTVOFF_EL2=0` +
  `CNTHCTL_EL2` so the guest timer works; an EL2 vector table; then
  `HCR_EL2.VM=1`.
- `linker.ld`: a `.hyp` section for hypervisor-private memory.
*Verified:* `CurrentEL=0x2`, stage-2 on, a test guest `HVC` traps to EL2 and
resumes; Fermi's own stage-1 MMU self-tests pass *nested*.

**Two bugs found here (great learning):**
1. **Oversized vector stubs.** My first EL2 vector entries were ~38 instructions
   (152 bytes), overflowing the 128-byte architectural slot, so the CPU landed
   on the wrong handler. Fixed with a compact stub + shared `el2_common`.
2. **`zero_bss` wiping the stage-2 tables.** I'd placed the tables in `.bss`; the
   guest's `early_init→zero_bss()` zeroed them right after the hypervisor built
   them → level-0 stage-2 fault. Fixed by carving a `.hyp` section *after*
   `__bss_end` (skipped by `zero_bss`) and *before* `__kernel_end` (reserved by
   the guest PMM).

### M2 — Hypercall ABI + trap-and-emulate
*Why:* build the EL2 trap infrastructure every later milestone needs.
- `hypercall.h`: SMCCC-like ABI (fn in x0, args x1–x3, result x0) shared by guest
  and hypervisor; calls VERSION/PUTC(paravirt console)/PING/VM_INFO/YIELD.
- `el2_dispatch`: clean switch on `ESR_EL2.EC` (HVC / trapped MSR-MRS / aborts).
- sysreg trap-and-emulate via `HCR_EL2.TID3` (decode ISS, emulate ID-register
  reads, step `ELR_EL2`). `vcpu_t` control block with stats.

### M3 — Stage-2 isolation of hypervisor memory
*Why:* make it a genuinely *isolating* hypervisor, not a flat identity map.
- Split the 1 GiB block holding `.hyp` down to 4 KiB and **unmap** the hyp pages
  from the guest. The abort handler treats a guest access to hyp memory as an
  isolation violation: report, poison (0) the destination on a read, step over.

### M4 — GICv3 virtual interrupt injection (vGIC)
*Why:* virtualize interrupts so guest interrupt handling runs on the virtual CPU
interface.
- Route physical IRQs to EL2 (`HCR_EL2.IMO`), bring up the EL2 physical CPU
  interface + the virtual CPU interface (`ICH_HCR_EL2`). On a physical IRQ, ack
  it and inject a **hardware-linked** virtual interrupt (`ICH_LR<n>.HW=1`) so the
  guest's unmodified handler EOIs it and auto-deactivates the physical one.
- *Key realization:* the guest's `ICC_*` CPU-interface accesses are
  transparently redirected by hardware to the virtual interface — only the
  distributor/redistributor MMIO needs emulation (later).
*Verified:* Fermi's preemptive scheduler runs entirely off injected virtual
timer IRQs.

### M5a — Second guest + cooperative world-switch
*Why:* go multi-VM. `vcpu_t` becomes a full context (GP regs, PC/PSTATE, EL1
sysregs, per-guest `VTTBR` with distinct VMID). `HVC_YIELD` saves the running
vCPU and resumes the next. A tiny position-independent payload is the second
guest, in its own isolated RAM slice carved from the hyp region.

### M5b — Preemptive scheduling (EL2 timer)
*Why:* preempt guests without cooperation. The hypervisor owns the **EL2 physical
timer `CNTHP_EL2` (PPI 26)** on a 100 ms quantum; each tick world-switches
round-robin. Guest 2 became a non-cooperative spinner to prove preemption.

### M6 — Per-guest vGIC state + interrupt ownership routing
*Why:* remove the M5b shared-list-register shortcut so the design is correct for
interrupt-driven guests. Save/restore `ICH_LR`/`ICH_VMCR`/`ICH_AP1R0` per vCPU;
route each physical INTID to its owning vCPU (inject into the owner's live or
*saved* list register).

### M7 — FP/SIMD context switching
*Why:* a guest must not corrupt another's `q`-registers.
- Add `q0..q31`/`FPSR`/`FPCR` to `vcpu_t`; save FP first / restore last in the
  world switch; clear `CPTR_EL2.TFP` so EL2 may run FP.
- *Verified robustly:* guest 1 parks a sentinel in `d5` and self-checks it every
  iteration; across hundreds of preemptions (while Fermi's printf clobbers FP) it
  never corrupts → printed `X` zero times.

### M8 — `/proc/vms` introspection
*Why:* make the multi-VM state visible. Introspection hypercalls
(`VM_COUNT`/`VM_STAT`) + a `/proc/vms` generator (run at EL1, which may `HVC`).
`cat /proc/vms` from Fermi's EL0 shell shows a live vCPU table.

### M9 — PSCI guest lifecycle
*Why:* let a guest power itself off (and it's a Linux prerequisite). PSCI over
`HVC` (`VERSION`, `SYSTEM_OFF`/`SYSTEM_RESET` → reap the vCPU). Guest 1 calls
`SYSTEM_OFF` after 6 iterations; the scheduler stops scheduling it (`UNUSED` in
`/proc/vms`).

### M10 — Large guest RAM region + Linux-slot stage-2
*Why:* a Linux guest needs lots of RAM, invisible to Fermi.
- QEMU RAM bumped to 10 GiB so there's a window **beyond Fermi's 8 GiB PMM view**
  (Fermi manages [1 GiB, 9 GiB); the guest region starts at 9 GiB / phys
  `0x240000000`). vCPU 1 repurposed into the "Linux slot": a 256 MiB→later 1 GiB
  stage-2 window mapping guest IPA `0x40000000` → high physical RAM, plus the
  PL011. Proven with a stub that runs from the high RAM and writes 'L' to the
  mapped UART.

### M11 — Boot a real Linux kernel to early init
*Why:* the goal. A hand-written `guest.dts` (memory, PL011 earlycon, PSCI-over-
HVC, armv8 timer, GICv3), compiled with `dtc`. QEMU's generic `loader` stages the
`Image` + DTB into the Fermi-invisible high RAM. The Linux vCPU enters per the
arm64 boot protocol (PC=Image, x0=DTB, EL1h, MMU off).
*Verified:* Linux prints earlycon, identifies the CPU, parses the cmdline, and
runs through memory init — **concurrently with Fermi**.

### M12 — Emulated GICv3 distributor + virtual timer; the `SP_EL0` fix
*Why:* Linux needs to program a GIC and get timer ticks.
- Emulated GICv3 distributor/redistributor MMIO (the Linux GIC region is left
  unmapped → traps to EL2): correct `PIDR2` (v3), `TYPER`, redistributor `WAKER`
  handshake / `Last` bit; config writes accepted; the data-abort ISS is decoded
  to emulate the load/store.
- Virtual timer: Linux uses `CNTV` (INTID 27) — *separate hardware* from Fermi's
  `CNTP` (30) and the scheduler's `CNTHP` (26), so three timers coexist with no
  conflict.

**The decisive bug — `SP_EL0`.** Both kernels (6.8 and 5.4) stormed on a
recurring fault reading `~0x7fffXX`. I read the *guest's* `ESR_EL1`/`FAR_EL1`
from EL2 and disassembled the faulting instruction. Root cause: **arm64 Linux
keeps the `current` task pointer in `SP_EL0`**, and the world switch saved
`SP_EL1` but **not `SP_EL0`**. Fermi tolerated this (it rewrites `SP_EL0` on
every EL0 entry); Linux relies on it persisting. Fermi's leftover EL0 user-stack
pointer (`USER_STACK_TOP = 0x800000` = 8 MiB) leaked into Linux, so Linux read
`current` through Fermi's stale pointer and stormed. **Adding `SP_EL0` to the
saved context was the single change that took Linux from "hangs in mm init" to
"boots its entire kernel."** It then reached the *expected* "VFS: Unable to
mount root fs" panic (no initramfs yet).

### M13 — Boot Linux to an interactive userspace shell
*Why:* finish the goal. A busybox initramfs (static aarch64 busybox + an `/init`)
is staged via QEMU's loader; the DTB gets `linux,initrd-start/end` + `rdinit`.
*Verified:* Linux runs `/init`, prints a banner + `uname -a`, and drops to a
BusyBox `~ #` shell — interleaved on the serial with Fermi's own shell. **Goal
achieved.**

**The SCS detour.** The first kernel tried (Ubuntu 6.8 generic) hung in a
nested-exception storm: it's built with `CONFIG_SHADOW_CALL_STACK`, and its
exception-handler prologue's `str x30, [x18]` (shadow-stack push) faulted on an
unmapped `x18`. The fix was to use an **SCS-free kernel** (Ubuntu 5.4 generic,
which predates the SCS default). Also: building a custom kernel was blocked by a
**100%-full host disk** (only ~1 GB free), so we fetched a small prebuilt
image-only `.deb` instead.

### M14 — Bidirectional guest RAM isolation
*Why:* full isolation. Linux's stage-2 only maps its own window (so it can't see
Fermi); we also **unmap the Linux RAM window from Fermi's stage-2** (clear the
1 GiB block) so Fermi can't see Linux.

### Cleanup + M15 — `/proc/vms` enrichment
Removed the now-unused M10 stub; added an `mmio` counter (separate from real
aborts) and guest names to `/proc/vms`, so the table reads sensibly: Fermi shows
timer vIRQs / 0 MMIO (it drives the physical GIC directly); Linux shows PSCI
hypercalls, virtual-timer vIRQs, and emulated GIC MMIO.

### Reproducibility + robustness
- `scripts/stage-linux-guest.sh`: fetches an SCS-free kernel `Image` and builds
  the busybox initramfs (handles `.deb` data members in xz/zstd), so a fresh
  checkout can reproduce the Linux boot. The Linux blobs stay gitignored.
- `/init` respawns the shell so exiting it doesn't panic the kernel (kill init).

### Dedicated Linux console (capture)
*Why:* the user picked this — Fermi and Linux interleaving on one serial is
noisy, and QEMU `virt` has only **one** PL011 (so no second interactive UART).
- Emulate the Linux PL011 (unmap it → trap): capture DR writes into a 32 KiB
  hypervisor ring buffer, exposed as **`cat /proc/linux_console`**; emulate
  FR/IDs so `ttyAMA0` binds. Fermi's serial stays clean; Linux's full log is
  viewable separately.

### M16 — Third guest (N>2 multi-VM)
*Why:* prove the vCPU table / scheduler / per-guest stage-2 generalize past two.
`NUM_VCPUS=3`; vCPU 2 is a tiny silent payload (VMID 2) in its own isolated slice
at phys 10 GiB. It loops issuing `HVC_PING`, so its progress shows up purely as a
climbing HVC count in `/proc/vms`.

### M17 — Interactive Linux console (emulated UART RX + SPI injection)
*Why:* restore input to the captured console. The keystone is **software SPI
injection** (`hyp_vgic_inject_ex(..., hw=0)`): an emulated device's interrupt
injected as a virtual (non-hardware-linked) SPI. Added a PL011 RX FIFO + the
FR/IMSC/RIS/MIS registers; on each tick, if input is pending and RXIM is set,
inject the UART SPI (INTID 33). Input API: `HVC_LCON_PUT` + a writable
`/proc/linux_console`.
*Verified:* Fermi queued `echo HVTEST_OK`; `HVTEST_OK` came back in
`/proc/linux_console`, proving the full path HVC_LCON_PUT → RX FIFO → injected
SPI → pl011 IRQ → tty → shell.

### M18 — Emulated virtio-rng (virtio-mmio)
*Why:* the virtio frontier, unblocked by M17's SPI injection. A full virtio-mmio
v2 device (DeviceID 4) at IPA `0x0a000000`: emulate the register set + walk the
guest's **split virtqueue in guest memory** (desc/avail/used, reached via the
Linux IPA→PA linear map) on `QueueNotify`, fill buffers with pseudo-random bytes,
publish the used ring, inject the device SPI (INTID 34). The staging script also
fetches `virtio-rng.ko` (the driver is `=m`; `virtio_mmio` is built in) and
`/init` insmods it.
*Verified:* Linux loads the driver and kicks the queue; the hypervisor logs
servicing the guest queue 3×.

### M19 — Emulated virtio-blk `/dev/vda`
*Why:* a real block device. A second virtio-mmio device (DeviceID 2) at
`0x0a000200` backed by a 256 KiB hypervisor RAM disk. On `QueueNotify` the
hypervisor walks each request's descriptor chain (out-header → data → status) and
reads/writes the RAM disk. The disk is seeded with a signature **and a minimal
MBR**.
*Verified — cleanly:* Linux registers `/dev/vda 512 512-byte logical blocks
(256 KiB)` (our exact capacity) and its partition scan reads sector 0 over the
virtqueue and detects `vda: vda1` — proving block reads return correct data,
*independent of the shell*.

### Interactive console: cursor-query terminal emulation (the real fix)
A long canned command first appeared truncated at the guest shell. I asserted
the PL011 RX-timeout interrupt (`RTMIS`) on a hunch — a correctness improvement,
but it did *not* fix the symptom. Instead of guessing further, I **traced the
exact bytes the guest reads** from the UART (a debug dump on each `DR` pop) and
found the guest received the *entire correct line* — so the hypervisor's RX
delivery was never the problem. The output stream contained `ESC[6n`: busybox's
line editor was sending a **cursor-position query** and blocking to read the
reply, eating real input when no terminal answered. Fix: the emulated PL011 now
**acts as a minimal terminal** — it detects `ESC[6n` in the guest's UART writes
and injects a cursor report (`ESC[1;1R`) into the RX FIFO; the demo command is
injected right after the first prompt query (so it can't be eaten). *Verified:*
the shell runs the full command and prints the `/dev/vda` signature
`VBLKOK_FERMI_HV`. Lesson: trace before theorizing — the bug was a console-
emulation gap, not byte loss.

### WFI idle-yield trapping
*Why:* with three guests round-robin on a 100 ms quantum, an idle guest's `WFI`
halts the physical CPU until the next interrupt — wasting the rest of its slice
while the others wait. Set `HCR_EL2.TWI=1` so guest `WFI` traps to EL2 (EC
`0x01`); the handler steps past the instruction and immediately world-switches
to the next runnable vCPU, donating the idle time. `WFE` is deliberately left
untrapped (no `TWE`) to avoid thrashing on guest spinlocks. The idle-yield count
is exposed via a new `VMSTAT_WFI` and shown as `idle-yields` in `/proc/vms`.
*Verified:* a normal run shows hundreds of idle-yields (e.g. `1161
world-switches, 362 idle-yields`) while Linux still boots and reads `/dev/vda`,
with no anomalies.

### Emulated virtio-net + ping (the hypervisor as link peer)
*Why:* give the guest a network interface — the most visible new capability —
while staying self-contained (no dependency on Fermi's NIC or QEMU slirp). A
virtio-mmio network device (DeviceID 1) at IPA `0x0a000400` (SPI 4 / INTID 36)
exposing `eth0`. Unlike rng/blk this needs **two virtqueues** (0 = RX, 1 = TX),
so the register model tracks the selected queue. The hypervisor itself is the
*other end of the wire*: on a TX notify it gathers the guest's ethernet frame
(skipping the 12-byte `virtio_net_hdr_v1`) and, for an ARP request or ICMP echo
to `10.0.0.1`, crafts a reply — reusing small in-hypervisor ARP/IPv4/ICMP +
RFC1071-checksum helpers — and scatters it into a posted RX buffer, injecting
the device SPI. `virtio_net.ko` (plus `failover`/`net_failover`) is shipped in
the initramfs. *Verified, first try:* `ifconfig eth0 10.0.0.2 up; ping -c 2
10.0.0.1` returns `2 packets transmitted, 2 packets received, 0% packet loss`
with `ttl=64` (the value the hypervisor stamps), exercising the complete
two-queue TX+RX path. No anomalies.

### SMP guest — a 2-core Linux
*Why:* the most "real hypervisor" milestone — give Linux more than one vCPU.
This touched many subsystems at once:
- **Per-vCPU identity**: added `mpidr` to the vCPU and set `VMPIDR_EL2` /
  `VPIDR_EL2` on every world-switch, so each core reads a distinct `MPIDR_EL1`
  (core0 aff0=0, core1 aff0=1) and a valid `MIDR`.
- **A second Linux vCPU** (vCPU2) sharing the primary's stage-2 (same
  VTTBR/VMID 1) — it replaced the old silent payload guest. Parked `UNUSED`
  until the boot CPU starts it.
- **PSCI `CPU_ON`/`AFFINITY_INFO`/`CPU_OFF`/`FEATURES`**: the boot CPU's
  `CPU_ON(mpidr, entry, ctx)` fills in the secondary's entry PC + context and
  marks it `READY`; the scheduler then runs it.
- **2-redistributor emulated GICR**: the GICR window holds two 0x20000 frames
  with per-core `GICR_TYPER` affinity and the `Last` bit on core1, so each core
  finds its own redistributor.
- **Per-vCPU virtual timer**: save/restore `CNTV_CTL/CVAL_EL0` and route the
  CNTV PPI to the *running* core.
- **The hard part — IPIs.** `SMP: Total of 2 processors activated` printed, but
  Linux then hung in `on_each_cpu` (an IPI). Cross-core IPIs are GICv3 SGIs;
  with one physical CPU they must be trapped and re-injected as virtual SGIs.
  Set `ICH_HCR_EL2.TC` to trap the guest's "common" CPU-interface registers,
  decode `ICC_SGI1R_EL1` writes (INTID + target affinity / IRM) and inject the
  SGI into the target core's vGIC. `TC` also traps `ICC_PMR_EL1` / `ICC_CTLR_EL1`,
  so those are mirrored into `ICH_VMCR_EL2` (a wrong VPMR would block *all*
  interrupt delivery — including the primary guest's, since `TC` is global —
  so this had to be exact). The hot IAR/EOIR path is group-1 (not under `TC`)
  and stays on the hardware virtual interface.
*Verified:* `nproc` returns **2**, `CPU1: Booted secondary processor` and
`SMP: Total of 2 processors activated` appear, and Linux runs all the way to
userspace (the IPI-driven `on_each_cpu` now completes — proving the secondary
receives SGIs, runs the work, and acks) with `/dev/vda` read and `ping` working
concurrently. The primary guest (Fermi) is unaffected by the new `TC` trapping.
No anomalies.

### A real ext4 disk on virtio-blk
*Why:* the natural step up from the 256 KiB signature disk toward a root disk —
back virtio-blk with a genuine, larger filesystem the guest can mount. The
256 KiB in-`.hyp` RAM disk is replaced by an **8 MiB ext4 image** staged by
QEMU's loader into Fermi-invisible high RAM at phys `0x280000000` (just past the
Linux window). `g_vdisk` becomes a pointer to that physical region (EL2 is
MMU-off), capacity 16384 sectors; the seeded MBR/signature is gone — the image
*is* the content. The image is built reproducibly with `mke2fs -d` (populate
from a directory, no loop mount) in the build container via a Makefile rule
(`CONFIG_EXT4_FS=y`, so no kernel module is needed). *Verified:* `EXT4-fs
(vda): mounted filesystem with ordered data mode`, and the guest reads back
`hello.txt` ("Hello from a real ext4 filesystem served by Fermi-HV
virtio-blk!") — all while SMP (`nproc` 2) and `ping` work concurrently. No
anomalies.

### True root disk — switch_root into /dev/vda (+ an SMP scheduling fix)
*Why:* finish the root-disk story — make `/dev/vda` Linux's actual root, not
just a mountable extra. The ext4 image is built as a full busybox rootfs (init,
applet symlinks, `/etc/motd`); the initramfs `/init` loads `virtio_blk`, mounts
`/dev/vda`, and `exec switch_root /newroot /sbin/init`. (`virtio_blk` is a
module, so the pivot must come from the initramfs rather than a built-in
`root=` mount.)
*The bug it surfaced:* the first attempt soft-locked — `watchdog: BUG: soft
lockup - CPU#0 stuck for 23s` in `finit_module`. Module loading uses
`stop_machine`, which needs *both* vCPUs synchronized; with a 100 ms quantum on
one physical CPU, the cross-core IPI that wakes the other CPU's stopper thread
arrived too late and CPU0 spun. Fix: shorten the scheduler quantum to **10 ms**,
so an SGI injected into a descheduled core is delivered within ~one short slice.
This not only fixed the lockup but sped the whole boot ~7× (IPI-heavy paths
dominate). *Verified:* `[init] mounted /dev/vda as ext4; switching root`,
`ROOTFS_ON_VDA: running from the ext4 root on /dev/vda`, the on-disk `/etc/motd`
printed by the pivoted init, then `nproc` 2 + `ping` working from the disk root.
No soft lockup, no anomalies. Lesson: time-sliced SMP guests are pathologically
sensitive to IPI latency — `stop_machine` is the canary.

### Live migration (local, pre-copy + dirty tracking)
*Why:* the migration *mechanism* — relocating a running guest's RAM + CPU state
transparently. True cross-host migration isn't possible here (one QEMU process,
no second hypervisor / transport), so this demonstrates the algorithm locally:
move a live guest's memory from one physical window to another and resume it
there, with the guest none the wiser.
- A small migratable guest (vCPU 3, `guest3.S`) increments a counter in its RAM
  every loop — observable proof of liveness/continuity. Its 2 MiB window has a
  **page-granular (4 KiB) stage-2** so individual pages can be write-protected.
- **Pre-copy:** write-protect all 512 pages, copy SRC→DEST while the guest runs.
- **Dirty tracking:** a guest write to a protected page faults to EL2; the
  handler marks that page dirty, re-grants write, TLBIs, and *re-executes* the
  store (ELR not advanced).
- **Iterative rounds:** repeat — re-copy only the pages dirtied since the last
  round, re-protect, let the guest run — until the dirty set is small ("converged")
  or a round cap is hit. The guest here has a 16-page working set, so each round
  re-copies ~16 pages and it hits the round cap (a faithful "busy guest" case).
- **Stop-and-copy:** final dirty flush, re-point the guest's stage-2 (`mig_l3`)
  SRC→DEST, TLBI, poison SRC.
The whole state machine is driven from the scheduler tick so the guest keeps
running between phases.
*A bug it surfaced — over-broad TLBI starves co-resident guests:* the first
iterative version used `tlbi alle1is` (invalidate ALL VMIDs) on every
write-fault and every round — 80+ full flushes that blew away Fermi's and
Linux's TLBs too, starving them until Fermi took a wild fault and parked (so
Linux never booted). Fix: scope invalidation to the migratable guest's VMID
(`tlbi ipas2e1is` + `vmalle1is` for a page; select VTTBR + `vmalls12e1is` for the
whole VMID). After that the other guests are untouched.
*Verified:* `pre-copy round 1..5 re-copied 0x10 (16) dirty page(s)` →
`round cap -> stop-copy … counter(DEST)=0x57` → `post-migrate counter climbing`
while `SRC poisoned=0xeeee…` — the counter is continuous across the move and the
guest provably runs from the destination. All concurrent with the SMP Linux
guest booting from its ext4 root disk. No anomalies.

---

## 2. The recurring debugging pattern (why it worked)

A theme across the hard bugs: **inspect the guest's state from EL2.** The
hypervisor can read the guest's `ESR_EL1`/`FAR_EL1`/`ELR_EL1`, log the guest's PC
at each preemption, and disassemble the guest `Image` at a faulting offset. That
turned three "Linux just hangs" mysteries into precise, fixable findings:
- vector-slot overflow (wrong handler) — from the reported vector index;
- the `SP_EL0`/`current` leak — from `FAR_EL1 ≈ 0x7fffXX` matching Fermi's user
  stack top, plus disassembling the faulting `str x30,[x18]`;
- the SCS storm — same technique, identifying the shadow-call-stack push.

Other decisions driven by honesty about constraints:
- **No VHE on A72** → separate EL2 hypervisor, Fermi as guest (not relocate).
- **One UART on `virt`** → captured `/proc/linux_console` instead of a fake
  second interactive UART.
- **100%-full host disk** → fetch a prebuilt SCS-free kernel rather than build.
- **Stock kernel ships virtio as modules** → insmod `virtio-rng.ko` /
  `virtio_blk.ko` from the initramfs (virtio_mmio/rng-core are built in).

---

## 3. What the system does today

`make run` (inside the `osdev` container, after `scripts/stage-linux-guest.sh`)
boots **Fermi-as-a-Type-1-hypervisor at EL2**, which runs **three isolated guests
under preemptive scheduling**:
- **vCPU 0:** Fermi OS itself, unmodified, to its interactive EL0 shell.
- **vCPU 1:** an unmodified aarch64 **Linux 5.4** kernel — to a BusyBox userspace
  shell — with an emulated **GICv3**, **virtual timer**, a **captured+interactive
  console** (`/proc/linux_console`), an emulated **virtio-rng**, an emulated
  **virtio-blk `/dev/vda`** that serves as Linux's **ext4 root filesystem**
  (the initramfs `switch_root`s into it), and an emulated **virtio-net `eth0`**
  (can `ping` the hypervisor host `10.0.0.1`).
- **vCPU 2:** Linux's **second core** (SMP secondary), brought online by the boot
  CPU via PSCI `CPU_ON`; `nproc` reports 2.
- **vCPU 3:** a small **migratable** guest that is live-migrated between two
  physical RAM windows (pre-copy + stage-2 dirty tracking + stop-and-copy).

Full per-guest context is switched: GP regs, PC/PSTATE, the EL1 system-register
bank (including `SP_EL0`), vGIC state, and FP/SIMD. Memory is mutually isolated
via per-guest stage-2 (distinct VMIDs).

---

## 4. Git branches (no PRs; `main` untouched)

Pushed to `git@github.com:rituparna-ui/fermi-os.git`:
- `feat/el2-type1-hypervisor-linux-guest` — M1–M15 (the core arc)
- `progress/fermi-hypervisor-m1-m15-…` — a snapshot
- `docs/hypervisor-design-…` — `HYPERVISOR.md`
- `feat/linux-init-respawn-…` — shell respawn
- `feat/dedicated-guest-consoles-…` — captured Linux console (`/proc/linux_console`)
- `feat/third-guest-nvm-…` — M16 third guest
- `feat/interactive-linux-console-…` — M17 RX + SPI injection
- `feat/virtio-rng-mmio-…` — M18
- `feat/virtio-blk-mmio-…` — M19
- `fix/pl011-rx-timeout-irq-…` — partial PL011 RX fix (this devlog likely lands here too)

(Some feature branches are stacked, so later ones contain earlier work.)

---

## 5. Known limitations / future work

- **Interactive console input** now works (including long commands). The earlier
  "byte loss" was *not* a delivery bug — the hypervisor delivered every byte
  correctly (proven by tracing the guest's UART reads). The real cause was that
  busybox's line editor sends `ESC[6n` (cursor-position query) and blocks reading
  the reply; with no terminal answering, it consumed real input. Fixed by having
  the emulated PL011 act as a minimal terminal: detect `ESC[6n` in the guest's
  UART output and reply with a cursor report (`ESC[1;1R`). Verified: the shell
  now runs `echo HVTEST_OK; head -c 16 /dev/vda; echo` correctly, printing
  `HVTEST_OK` and the `/dev/vda` signature `VBLKOK_FERMI_HV`.
- **Single physical CPU**; guests are one vCPU each (the DT advertises 1 CPU to
  Linux).
- Emulated **GICv3** covers what the boot path needs (PPIs via injection +
  identity distributor reads + software SPI injection for emulated devices); full
  SPI routing/priority for arbitrary devices isn't modelled.
- Requires an **SCS-free** guest kernel (the staging script fetches one).
- **virtio-blk is the guest's root device.** `/dev/vda` is an 8 MiB ext4 image
  containing a busybox rootfs; the initramfs loads `virtio_blk`, mounts it, and
  `switch_root`s into it, so Linux runs entirely from the hypervisor's emulated
  disk (`ROOTFS_ON_VDA`). Reads and writes are both exercised.
- Building a custom kernel here is blocked by host disk space.

---

## 6. How to reproduce

```bash
# On a host with internet (fetches an SCS-free Image + builds the busybox initramfs):
./scripts/stage-linux-guest.sh

# Inside the osdev build container (aarch64 toolchain + QEMU; project at /mnt/fermi):
make run
# -> Fermi boots at EL2 as the hypervisor, runs Fermi (guest0) + Linux (guest1)
#    + a payload (guest2). In Fermi's shell:
#       cat /proc/vms             # live vCPU table
#       cat /proc/linux_console   # the Linux guest's boot log + shell
```
