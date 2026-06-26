#ifndef HYP_HYP_H
#define HYP_HYP_H

#include <stdint.h>

/* FermiOS EL2 hypervisor — milestone 2: dedicated EL2 trap plumbing.
 *
 * The host kernel runs at EL2 under VHE (HCR_EL2.E2H=1, set in boot.S). Its
 * own exceptions go through the redirected VBAR_EL1 (== VBAR_EL2) table in
 * src/exception/vector.S. This module provides a SEPARATE EL2 vector table
 * that reads the EL2-banked syndrome registers (ELR/SPSR/ESR/HPFAR/FAR_EL2)
 * and is the future guest-exit hot path: the world switch will install it in
 * VBAR_EL2 before entering a guest and restore the host table on exit. */

/* Exception type tags passed by the vector stubs (mirror EXCEPTION_* in
 * exception.h so the two dispatchers stay legible side by side). */
#define HYP_EXC_SYNC   0
#define HYP_EXC_IRQ    1
#define HYP_EXC_FIQ    2
#define HYP_EXC_SERROR 3

/* EL2 trap frame. MUST match the store/load offsets in hyp_vectors.S.
 *   regs[0..30]  x0..x30            (248 bytes)
 *   elr          ELR_EL2            (+248)
 *   spsr         SPSR_EL2           (+256)
 *   esr          ESR_EL2            (+264)
 *   hpfar        HPFAR_EL2          (+272)  faulting IPA[47:12]<<4 on S2 abort
 *   far          FAR_EL2            (+280)
 * Total 288 bytes (16-byte aligned). */
typedef struct hyp_trap_frame {
  uint64_t regs[31];
  uint64_t elr;
  uint64_t spsr;
  uint64_t esr;
  uint64_t hpfar;
  uint64_t far;
} hyp_trap_frame_t;

#define HYP_FRAME_SIZE 288

/* True if currently executing at EL2 (the VHE host); false at EL1 (legacy
 * boot or running as a guest). */
int hyp_at_el2(void);

/* Bring up the EL2 trap layer: probe EL2/VHE, and (milestone 2) run a
 * self-test that routes a host-issued HVC through the dedicated EL2 vector
 * table to prove syndrome capture works and the host survives. No-op at EL1. */
void hyp_init(void);

/* C entry from hyp_exception_common. type is one of HYP_EXC_*. */
void hyp_dispatch(uint64_t type, hyp_trap_frame_t *frame);

/* The dedicated EL2 vector table (defined in hyp_vectors.S). */
extern char hyp_vector_table[];

/* Milestone 3 smoke test: build a stage-2, load the trivial EL1 guest stub,
 * world-switch into it, and verify the HVC exit + guest RAM write. No-op at
 * EL1. Must run before the host timer is started (it uses no preemption). */
void hyp_run_smoke_guest(void);

/* Milestone 4 demo: run a spinning EL1 guest preempted by the EL2 physical
 * timer (CNTHP, PPI 26) for a fixed number of slices, proving IRQ-driven
 * exit + guest resume. No-op at EL1. Runs before the host timer starts. */
void hyp_run_timeslice_demo(void);

/* Milestone 7/8: load the embedded (reduced-RAM) FermiOS guest image into a
 * PMM-backed IPA window, build its stage-2 (RAM + straight-through UART), and
 * run it as an EL1 guest, reporting how far it boots and the first trap it
 * takes. No-op at EL1. Runs before the host timer starts. */
void hyp_boot_fermios_guest(void);

/* Milestone 9: round-robin TWO heartbeat EL1 guests, each with its own VMID,
 * stage-2 address space and RAM, scheduled on the EL2 timer. Proves per-vCPU
 * GP-context isolation + stage-2 isolation. No-op at EL1; runs before the host
 * timer starts. */
void hyp_run_multi_guest_demo(void);

/* Milestone 9c: preemptively round-robin TWO full FermiOS guests, each with its
 * own VMID/stage-2/RAM/vGIC, time-sliced on the EL2 physical timer with full
 * per-vCPU context save/restore. No-op at EL1; runs before the host timer. */
void hyp_run_dual_fermios(void);

/* Milestone 11: PSCI self-test. Run a guest that issues PSCI SYSTEM_RESET and
 * verify the hypervisor warm-resets it (guest re-enters at its entry, counter
 * cleared). No-op at EL1; runs before the host timer. */
void hyp_run_psci_test(void);

/* Milestone 12: run ONE FermiOS guest interactively — route physical UART input
 * to the guest's virtual console so you can type at its EL0 shell. Does not
 * return (owns the console). No-op at EL1. */
void hyp_run_interactive_guest(void);

/* Milestone 14: run TWO full FermiOS guests preemptively (round-robin on the
 * EL2 timer) AND interactively — host console input is routed to the FOCUSED
 * guest, and Ctrl-X cycles which guest's shell has focus. Does not return.
 * No-op at EL1. */
void hyp_run_multi_interactive(void);

/* Milestone 15: demonstrate inter-VM shared memory + a doorbell hypercall.
 * One host page is mapped into BOTH guests' stage-2 at a fixed IPA; a producer
 * guest writes it and rings a doorbell (HVC), and a consumer guest reads it.
 * No-op at EL1; runs before the host timer. */
void hyp_run_shm_doorbell(void);

/* Milestone 16: interrupt-driven inter-VM doorbell. The producer's doorbell
 * hypercall injects a virtual SPI into the peer (consumer) guest's vGIC; the
 * consumer takes the IRQ, reads the shared page in its handler, and EOIs.
 * No-op at EL1; runs before the host timer. */
void hyp_run_doorbell_irq(void);

/* Milestone 17: exercise the unified HVC hypercall ABI. Two demo guests call
 * VERSION / VM_INFO / PUTC / YIELD through hvc_dispatch(); their PUTC output
 * appears via their per-guest virtual consoles. No-op at EL1; runs before the
 * host timer. */
void hyp_run_hvc_abi(void);

/* Milestone 19: paravirtualized block device. A guest reads a real host-disk
 * sector via the BLK hypercalls; the hypervisor stage-2-translates the guest's
 * buffer IPA to a host PA and drives the physical virtio-blk device. Proves
 * safe guest-buffer (DMA-equivalent) access. No-op at EL1; runs before the
 * host timer. */
void hyp_run_blk_pv(void);

/* Milestone 20: paravirtualized network device. A guest queries the NIC MAC,
 * sends an ARP probe and polls for the reply via the NET hypercalls; the
 * hypervisor stage-2-translates the guest's frame buffer and drives the real
 * virtio-net. Proves guest-driven NIC I/O. No-op at EL1; runs before the host
 * timer. */
void hyp_run_net_pv(void);

/* Milestone 21: dynamic VM lifecycle. Repeatedly create, run, and destroy a
 * guest VM, verifying that stage-2 teardown + RAM/VMID release return the PMM
 * free-page count to baseline (no leak across create/destroy cycles). No-op at
 * EL1; runs before the host timer. */
void hyp_run_vm_lifecycle(void);

/* Milestone 22: boot a FOREIGN (non-FermiOS) guest via the standard AArch64
 * boot protocol. The hypervisor builds a DTB into guest RAM, enters the
 * standalone mini-guest with x0 = DTB IPA, and the guest discovers its UART +
 * RAM by parsing the device tree. No-op at EL1; runs before the host timer. */
void hyp_run_miniguest(void);

/* Milestone 23: SMP guest. One VM with TWO vCPUs sharing a single stage-2
 * address space and distinct VMPIDRs. The boot vCPU brings up the secondary via
 * PSCI CPU_ON; both run and print their distinct MPIDR affinity. No-op at EL1;
 * runs before the host timer. */
void hyp_run_smp_guest(void);

#endif /* HYP_HYP_H */
