# Fermi-RMM — a minimal Arm CCA-style Realm Management Monitor

Fermi-RMM is a learning re-fork of [Fermi OS](https://github.com/rituparna-ui/fermi-os)
that turns its EL2 layer into a minimal **Realm Management Monitor (RMM)** in the
spirit of the Arm Confidential Compute Architecture (CCA). It is built from
scratch in C and aarch64 assembly, targets QEMU's `virt` machine, and boots the
existing Fermi kernel as the **Normal-world host** that drives the monitor.

It was forked from Fermi OS at the commit that first enforces **stage-2
isolation of monitor memory** (`feat(hyp): Enforce stage-2 isolation of
hypervisor memory (M3)`) — the natural seed for a confidential-computing
monitor — and grown milestone by milestone (R0–R5).

---

## The honest constraint

A *real* CCA RMM runs at **Realm-EL2** and only exists because **FEAT_RME**
hardware provides four physical security states (Root / Secure / Realm /
Non-secure), Granule Protection Checks (GPC/GPT), and an **EL3 monitor** (e.g.
TF-A) that programs the GPT and boots the RMM.

This project has **no FEAT_RME and no EL3 monitor**. Instead the RMM runs at
**EL2** and uses **stage-2 translation** to enforce isolation:

* memory owned by the monitor or a realm is **unmapped from the Normal world's
  stage-2**, so the host physically cannot read or write it;
* each realm gets its **own stage-2 tree** (the RTT) and a distinct VMID.

This faithfully reproduces *every RMM concept and control-flow* — granule
delegation, realm/RTT/REC lifecycle, world switch, RSI run loop, measurement
and attestation — while running on stock QEMU. What it does **not** model is the
hardware exclusion of the privileged host from realm memory by RME; here that
boundary is enforced by stage-2 instead of by a separate world. See
[Future work](#future-work) for the real-RME path.

---

## Worlds and components

```
            EL1  Normal world (host)            EL1  Realm (guest)
        +---------------------------+       +-----------------------+
        |  Fermi kernel  (VMID 0)   |       |  realm payload (VMID N)|
        |  drives RMI via `hvc`     |       |  calls RSI via `hvc`   |
        +-------------+-------------+       +-----------+-----------+
                      |  RMI (host -> RMM)              |  RSI (realm -> RMM)
                      v                                 v
        ============================ EL2 ============================
        |                Realm Management Monitor (RMM)             |
        |  granule state machine | realm/RTT/REC | world switch     |
        |  stage-2 (host VMID 0) + per-realm RTT | SHA-256 / RIM    |
        =============================================================
```

* **Host (EL1, VMID 0)** — the Fermi kernel. Owns most of RAM, drives the RMM
  through the **RMI** ABI to create and run realms.
* **RMM (EL2)** — this project. Owns the granule state table, realm/REC tables,
  every stage-2 tree, and the world switch. Its own memory is hole-punched out
  of the host's stage-2.
* **Realm (EL1, VMID N)** — a measured payload running under its own stage-2
  (RTT). Talks only to the RMM via **RSI**; it has no direct device access.

Source layout (all under `src/rmm/`):

| File | Role |
|---|---|
| `rmm.c` | monitor core: init, stage-2, granule SM, realm/RTT/REC, world switch, traps |
| `rmm.h` | EL2 register/descriptor defs, `vcpu_t` world-switch context, `el2_frame_t` |
| `rmi.h` | **RMI** ABI (host → RMM) + host-side `rmi_call`/`rmi_call4` trampolines |
| `rsi.h` | **RSI** ABI (realm → RMM) + realm-side `rsi_call` trampoline |
| `measure.{c,h}` | SHA-256 (FIPS 180-4) + `rim_extend` |
| `realm_payload.S` | the demo realm: prints via RSI, host-calls, attests, exits |
| `vector_el2.S` | EL2 exception vectors → `el2_dispatch` |

---

## Concept mapping to real CCA

| Real Arm CCA | Fermi-RMM |
|---|---|
| RMM at Realm-EL2 | RMM at **EL2** (no RME) |
| Host RMI via `smc` (routed by EL3) | Host RMI via `hvc` (no EL3) |
| Realm RSI via `smc` | Realm RSI via `hvc` |
| Granule Protection (GPT/GPC) excludes host | **Stage-2 unmap** excludes host |
| Realm Descriptor (RD) granule | `realm_t`, keyed by the RD granule PA |
| RTT (Realm Translation Table) | per-realm stage-2 tree (`rtt_l0` + pool) |
| REC (Realm Execution Context) | `rec_t` (`vcpu_t` world-switch context) |
| RIM (Realm Initial Measurement) | `realm_t.rim`, SHA-256 hash chain |
| Signed CCA attestation token | `SHA-256(RIM ‖ challenge)` (unsigned) |

---

## RMI — Realm Management Interface (host → RMM)

SMCCC-like: command FID in `x0`, args in `x1..x4`, status/result in `x0`.

| Command | Args | Effect |
|---|---|---|
| `RMI_VERSION` | — | ABI version |
| `RMI_FEATURES` | — | feature bitmap (stub) |
| `RMI_PUTC` | char | debug console putc |
| `RMI_PING` | v | returns v+1 (liveness) |
| `RMI_MONITOR_INFO` | — | # RMI calls serviced |
| `RMI_MONITOR_BASE` | — | base IPA of RMM-private region (isolation demo) |
| `RMI_GRANULE_DELEGATE` | pa | host → RMM; unmaps page from host stage-2 |
| `RMI_GRANULE_UNDELEGATE` | pa | RMM → host; **scrubs** then remaps page |
| `RMI_REALM_CREATE` | rd, rtt_base | new realm from two DELEGATED granules |
| `RMI_RTT_MAP` | rd, ipa, data | map a DELEGATED page into the realm IPA space |
| `RMI_RTT_READ_ENTRY` | rd, ipa | software-walk: returns mapped PA |
| `RMI_REALM_RIM` | rd, out_pa | copy the realm's RIM to a host buffer |
| `RMI_DATA_CREATE` | rd, data, ipa, src | copy host content into a granule, map it, **measure** it |
| `RMI_REC_CREATE` | rd, rec, entry_ipa | DELEGATED granule → REC with entry PC |
| `RMI_REC_ENTER` | rec | **world switch** into the realm; returns an exit reason |

REC exit reasons: `REC_EXIT_HOST_CALL`, `REC_EXIT_ABORT`, `REC_EXIT_DONE`.

## RSI — Realm Services Interface (realm → RMM)

| Command | Args | Serviced | Effect |
|---|---|---|---|
| `RSI_VERSION` | — | in place | RSI ABI version |
| `RSI_REALM_CONFIG` | — | in place | realm config (VMID here) |
| `RSI_PUTC` | char | in place | realm paravirt console |
| `RSI_HOST_CALL` | arg | **exit to host** | host-mediated request |
| `RSI_EXIT` | — | **exit to host** | realm finished |
| `RSI_ATTESTATION_TOKEN` | challenge | in place | `SHA-256(RIM ‖ challenge)`; low 64 bits in `x0` |

"In place" RSI calls are handled at EL2 and the realm is resumed immediately
(no host round trip). The host is only re-entered for `RSI_HOST_CALL` / `RSI_EXIT`
or a fault.

---

## Granule state machine

Every 4 KiB page the host hands to the monitor is tracked in a granule table
(RMM-private). Transitions are host-driven over RMI and enforced by stage-2:

```
UNDELEGATED --RMI_GRANULE_DELEGATE--> DELEGATED --+--> RD    (RMI_REALM_CREATE)
     ^                                            +--> RTT   (RMI_REALM_CREATE)
     |                                            +--> DATA  (RMI_DATA_CREATE / RTT_MAP)
     +-------RMI_GRANULE_UNDELEGATE (scrub)-------+--> REC   (RMI_REC_CREATE)
                                          (only DELEGATED is undelegatable)
```

A DELEGATED-or-beyond granule is unmapped from the host's stage-2; any host
access faults to EL2 and is reported as an isolation block (read poisoned to 0).
Undelegation **scrubs** the page before returning it, so no realm/monitor data
leaks back to the Normal world.

## Realm lifecycle (host view)

```
delegate(rd); delegate(rtt);              REALM_CREATE(rd, rtt)   -> RIM = 0
delegate(code); DATA_CREATE(rd, code, ipa, payload)              -> RIM extended
delegate(rec);  REC_CREATE(rd, rec, entry_ipa)
loop: REC_ENTER(rec)
        -> realm runs under its own stage-2 (VMID N)
        -> RSI serviced in place (resume) OR host-call/exit (return)
```

## World switch (`RMI_REC_ENTER` / realm trap)

`vcpu_t` captures a full EL1 world: GP regs (from the EL2 trap frame), `pc`/
`pstate` (`ELR_EL2`/`SPSR_EL2`), `vttbr` (stage-2 base | VMID), and the EL1
system-register set. `REC_ENTER` saves the host into `g_vcpu`, loads the REC,
swaps `VTTBR_EL2`, and erets into the realm. `g_running_rec` marks that a realm
is live, so `el2_dispatch` routes the next trap to the realm-exit/RSI path
instead of treating it as a host RMI call. Distinct VMIDs mean no stage-2 TLB
flush on the swap. (The save/restore structure is borrowed from the Fermi OS
M5a cooperative hypervisor.)

## Measurement & attestation

* The RIM starts at 32 zero bytes at `REALM_CREATE`.
* Each `RMI_DATA_CREATE` extends it: `RIM = SHA-256(RIM ‖ ipa_le64 ‖ page)`.
* `RSI_ATTESTATION_TOKEN(challenge)` returns `SHA-256(RIM ‖ challenge)`.
* The host (acting as a verifier) recomputes the RIM over the payload with the
  same `rim_extend` and asserts it matches — proving the realm booted exactly
  the intended code. A production token would additionally be COSE/CBOR-wrapped
  and **signed** by an attestation key.

---

## Build & run

Uses the same toolchain as Fermi OS (`aarch64-linux-gnu-` + QEMU). With the
provided dev container:

```bash
docker exec -it osdev bash          # aarch64 toolchain + qemu pre-installed
cd /mnt/rmm                          # or wherever the repo is mounted
make                                 # build build/kernel.elf
make rmm-demo                        # boot the EL2 RMM + realm demo in QEMU
```

`make rmm-demo` requires QEMU started with `virt,virtualization=on` (already the
default in the Makefile) so the image is entered at EL2. Quit QEMU with
`Ctrl-A` then `X`.

Expected early-boot output (abridged):

```
[RMM] Fermi RMM online at EL2
[RMM] REALM_CREATE rd=... rtt=... vmid=0x1
[RMM] DATA_CREATE ... RIM now 5771275f...6a85a377
[BOOT] realm RIM[0..7]=...  expected=...  -> MATCH (realm identity verified)
[RMM] REC_ENTER -> entering realm vmid=0x1
[realm] phase 1: running under my own stage-2 (RSI_PUTC)
[RMM] realm RSI_HOST_CALL arg=0xc0de -> exit to host
[BOOT]   host: serviced realm HOST_CALL, re-entering
[realm] phase 2: resumed by host re-entry; requesting attestation
[RMM] ATTESTATION vmid=0x1 challenge=0xcafe
[RMM]   RIM   5771275f...6a85a377
[RMM]   token 5672f4ee...83345030
[realm] received attestation token from RMM; exiting
[RMM] realm RSI_EXIT -> realm finished
```

After the demo the Fermi host continues its normal boot.

---

## Milestones

| Tag | Commit subject | What it adds |
|---|---|---|
| R0 | Reframe EL2 layer as Realm Management Monitor | rename hyp→rmm; behavior identical |
| R1 | Introduce the RMI host ABI | `rmi.h`, RMI dispatch |
| R2 | Granule state machine + RMI_GRANULE_DELEGATE | delegate/undelegate + scrub, dynamic stage-2 unmap |
| R3 | Realm Descriptor + RTT | per-realm stage-2; REALM_CREATE / RTT_MAP |
| R4a | REC + world switch into a realm | DATA_CREATE / REC_CREATE / REC_ENTER; realm executes |
| R4b | RSI realm ABI + re-entry run loop | `rsi.h`; in-place services + host-call/resume/exit |
| R5 | Measurement (RIM) + attestation | SHA-256, RIM chain, attestation token, host verify |

---

## Future work

* **`RMI_REALM_DESTROY`** — tear a realm down and reclaim/scrub all its
  granules (RD/RTT/REC/DATA → UNDELEGATED).
* **Two concurrent realms** — prove inter-realm isolation (distinct VMIDs and
  RTTs; one realm cannot reach another's IPA space).
* **Host-delegated RTT levels** — replace the RMM-private RTT pool with
  `RMI_RTT_CREATE` so intermediate tables are themselves delegated granules
  (as in the real spec).
* **Signed attestation** — wrap the token as COSE/CBOR signed by an attestation
  key, so a remote verifier can check it without trusting the channel.
* **Real FEAT_RME** — add a minimal EL3 monitor (GPT/GPCCR setup, four-world
  SMC routing, `RMM_BOOT`) and run the RMM at Realm-EL2 on QEMU
  `-cpu max,x-rme=on`. This is the faithful hardware path and a substantially
  larger effort.
