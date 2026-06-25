//! Minimal SMP: bring up a secondary core via PSCI CPU_ON.
//!
//! The secondary runs with the MMU off (it reports its MPIDR and parks), so
//! it must not touch the upper-half MMIO alias. Cross-core handshake uses
//! plain volatile stores/loads to BSS (no atomics — exclusive ops are illegal
//! on the secondary's Device-typed pre-MMU memory).

use crate::kprintln;
use crate::mm::mmu::KERNEL_VA_OFFSET;
use crate::mrs;
use core::arch::global_asm;

global_asm!(include_str!("smp.S"));

extern "C" {
    fn secondary_start();
}

#[repr(C, align(64))]
struct SecStack([u8; 8192]);
#[no_mangle]
static mut SECONDARY_STACK: SecStack = SecStack([0; 8192]);
// `secondary_stack_top` symbol for the asm: defined via a const address below.
core::arch::global_asm!(
    ".globl secondary_stack_top\n.set secondary_stack_top, SECONDARY_STACK + 8192"
);

static mut SECONDARY_UP: u64 = 0;
static mut SECONDARY_MPIDR: u64 = 0;
static mut SECONDARY_HEARTBEAT: u64 = 0;

const PSCI_CPU_ON: u64 = 0xC400_0003;

/// Entry point for the secondary core (MMU off, physical PC).
#[no_mangle]
pub extern "C" fn rust_secondary() -> ! {
    let mpidr: u64 = mrs!(mpidr_el1);
    unsafe {
        core::ptr::write_volatile(core::ptr::addr_of_mut!(SECONDARY_MPIDR), mpidr);
        core::ptr::write_volatile(core::ptr::addr_of_mut!(SECONDARY_UP), 1);
    }
    // Run a continuous heartbeat so the primary can observe the secondary
    // executing over time. MMU is off here, so use plain volatile stores
    // (no atomics / no upper-half MMIO).
    let mut beat: u64 = 0;
    loop {
        beat = beat.wrapping_add(1);
        unsafe {
            core::ptr::write_volatile(core::ptr::addr_of_mut!(SECONDARY_HEARTBEAT), beat);
        }
        // Modest delay between beats so the counter is human-readable.
        for _ in 0..2_000_000u64 {
            core::hint::spin_loop();
        }
    }
}

/// Current secondary-core heartbeat (0 if not online).
pub fn heartbeat() -> u64 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SECONDARY_HEARTBEAT)) }
}

/// Secondary core MPIDR (0 if not online).
pub fn secondary_mpidr() -> u64 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SECONDARY_MPIDR)) }
}

/// Whether the secondary reported online.
pub fn secondary_online() -> bool {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SECONDARY_UP)) != 0 }
}

/// Bring up core 1 (PSCI CPU_ON) and wait for it to report online.
pub fn bringup() {
    let entry_phys = (secondary_start as usize as u64).wrapping_sub(KERNEL_VA_OFFSET);
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "hvc #0",
            inout("x0") PSCI_CPU_ON => ret,
            in("x1") 1u64,          // target MPIDR (core 1, Aff0=1)
            in("x2") entry_phys,    // physical entry point
            in("x3") 0u64,          // context id
            options(nomem, nostack)
        );
    }
    if ret != 0 {
        kprintln!("[SMP] PSCI CPU_ON failed: {}", ret as i64);
        return;
    }
    // Wait (bounded) for the secondary to set its flag.
    for _ in 0..50_000_000u64 {
        let up = unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SECONDARY_UP)) };
        if up != 0 {
            let m = unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SECONDARY_MPIDR)) };
            kprintln!("[SMP] secondary core online, MPIDR={:#x} (Aff0={})", m, m & 0xFF);
            return;
        }
        core::hint::spin_loop();
    }
    kprintln!("[SMP] secondary did not come online (single-core?)");
}
