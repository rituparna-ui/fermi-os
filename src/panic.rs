//! Kernel panic: mask interrupts, dump system registers, halt.
//!
//! Port of the original `src/lib/panic/panic.c`.

use crate::kprintln;
use crate::mrs;

/// Unrecoverable error: dump diagnostics and park the core forever.
pub fn kernel_panic(msg: &str) -> ! {
    // Mask all interrupts so a pending IRQ can't re-enter during the dump.
    unsafe { core::arch::asm!("msr daifset, #0xf", options(nomem, nostack)) };

    let caller_lr: u64;
    unsafe { core::arch::asm!("mov {}, lr", out(reg) caller_lr) };

    kprintln!("");
    kprintln!("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    kprintln!("!!!         KERNEL PANIC            !!!");
    kprintln!("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    kprintln!("");
    kprintln!("  Reason: {}", msg);

    let elr: u64 = mrs!(elr_el1);
    let esr: u64 = mrs!(esr_el1);
    let far: u64 = mrs!(far_el1);
    let spsr: u64 = mrs!(spsr_el1);
    let sp: u64;
    unsafe { core::arch::asm!("mov {}, sp", out(reg) sp) };

    kprintln!("");
    kprintln!("  ELR_EL1  (return addr) : {:#x}", elr);
    kprintln!("  ESR_EL1  (syndrome)    : {:#x}", esr);
    kprintln!("  FAR_EL1  (fault addr)  : {:#x}", far);
    kprintln!("  SPSR_EL1 (saved state) : {:#x}", spsr);
    kprintln!("  SP       (stack ptr)   : {:#x}", sp);
    kprintln!("  LR       (caller pc)   : {:#x}", caller_lr);
    kprintln!("");
    kprintln!("  System halted. Reset to continue.");

    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}
