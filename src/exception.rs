//! Minimal diagnostic exception handling (interim).
//!
//! Installs a vector table that, on any exception, dumps the entry index,
//! ESR/ELR/FAR/SPSR via the Device-safe `uart` helpers (never core::fmt), then
//! halts. The full trap-frame + dispatch implementation lands at the
//! exceptions milestone.

use crate::mrs;
use crate::uart;
use core::arch::global_asm;

global_asm!(include_str!("exception/vector.S"));

extern "C" {
    static vector_table: u8;
}

/// Install the vector table into VBAR_EL1.
pub fn init() {
    let vbar = unsafe { &vector_table as *const u8 as u64 };
    crate::msr!(vbar_el1, vbar);
    unsafe { core::arch::asm!("isb") };
    uart::log_hex("[EXC] vector_table @ ", vbar);
}

#[no_mangle]
pub extern "C" fn rust_exception_entry(index: u64) -> ! {
    let esr: u64 = mrs!(esr_el1);
    let elr: u64 = mrs!(elr_el1);
    let far: u64 = mrs!(far_el1);
    let spsr: u64 = mrs!(spsr_el1);
    let ec = (esr >> 26) & 0x3F;
    uart::println("");
    uart::println("========== EXCEPTION (diag) ==========");
    uart::log_dec("  vector index : ", index);
    uart::log_hex("  ESR_EL1      : ", esr);
    uart::log_hex("  EC (class)   : ", ec);
    uart::log_hex("  ELR_EL1      : ", elr);
    uart::log_hex("  FAR_EL1      : ", far);
    uart::log_hex("  SPSR_EL1     : ", spsr);
    uart::println("======================================");
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}
