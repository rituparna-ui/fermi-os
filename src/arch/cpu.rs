//! CPU/exception-level helpers.
//!
//! At this stage this mirrors the C `print_current_el` path. The full CPU
//! identification + PMU cycle-counter support (originally `cpu.c`) is ported
//! later, following the original commit progression.

use crate::klib::uart::Uart;
use crate::mrs;

/// The current exception level, read from `CurrentEL[3:2]`.
pub fn current_el() -> u8 {
    let current_el = mrs!("CurrentEL");
    ((current_el >> 2) & 0b11) as u8
}

/// Human-readable name for an exception level.
pub fn el_name(el: u8) -> &'static str {
    match el {
        0 => "User Space",
        1 => "Kernel Space",
        2 => "Hyper Space",
        3 => "Secure Monitor/Firmware",
        _ => "Invalid Exception Level",
    }
}

/// Print the current exception level over the UART.
///
/// Uses the UART's direct string helpers rather than `kprintln!`. Before the
/// MMU is enabled, RAM is treated as Device memory (strongly-ordered), so the
/// unaligned 2-byte accesses that `core::fmt`'s integer formatting emits fault
/// — and no exception vectors are installed yet. The aligned `puts`/`putc`
/// path is safe; `kprintln!`/`core::fmt` become usable once the MMU maps RAM as
/// Normal cacheable memory.
pub fn print_current_el() {
    let el = current_el();
    let uart = Uart;
    uart.puts("Current Exception Level: ");
    uart.println(el_name(el));
}
