//! Kernel panic handling: register dump + CPU halt.
//!
//! `kernel_panic` mirrors the C path — mask all interrupts, dump the key
//! system registers, and park the CPU in `wfi`. The Rust `#[panic_handler]`
//! prints the panic message/location and routes into the same halt path.

use crate::klib::uart::Uart;
use crate::{kprintln, mrs};
use core::panic::PanicInfo;

/// Unrecoverable error: dump diagnostic state and halt forever.
pub fn kernel_panic(msg: &str) -> ! {
    // Mask all interrupts (D, A, I, F) so a pending IRQ can't re-enter the
    // exception path during the dump and recurse.
    unsafe {
        core::arch::asm!("msr daifset, #0xf", options(nomem, nostack));
    }

    // Capture the caller's return address (AAPCS64 x30) before any call below.
    let caller_lr: u64;
    unsafe {
        core::arch::asm!("mov {}, x30", out(reg) caller_lr, options(nomem, nostack));
    }

    let uart = Uart;
    uart.println("");
    uart.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    uart.println("!!!         KERNEL PANIC            !!!");
    uart.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    uart.println("");

    kprintln!("  Reason: {}", msg);

    let elr = mrs!("elr_el1");
    let esr = mrs!("esr_el1");
    let far = mrs!("far_el1");
    let spsr = mrs!("spsr_el1");
    let sp: u64;
    unsafe {
        core::arch::asm!("mov {}, sp", out(reg) sp, options(nomem, nostack));
    }

    kprintln!("");
    kprintln!("  ELR_EL1  (return addr) : {:#x}", elr);
    kprintln!("  ESR_EL1  (syndrome)    : {:#x}", esr);
    kprintln!("  FAR_EL1  (fault addr)  : {:#x}", far);
    kprintln!("  SPSR_EL1 (saved state) : {:#x}", spsr);
    kprintln!("  SP       (stack ptr)   : {:#x}", sp);
    kprintln!("  LR       (caller pc)   : {:#x}", caller_lr);
    kprintln!("\n  System halted. Reset to continue.");

    halt();
}

/// Park the CPU with interrupts masked.
fn halt() -> ! {
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}

#[panic_handler]
fn rust_panic(info: &PanicInfo) -> ! {
    // core::fmt is safe here: panics only happen after the MMU is up in
    // practice, and the message path uses the UART formatter.
    let uart = Uart;
    uart.println("");
    uart.println("!!! RUST PANIC !!!");
    kprintln!("  {}", info);
    halt();
}
