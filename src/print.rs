//! Formatted kernel output.
//!
//! The original C kernel grew a variadic `uart_printf` a few commits after the
//! PMM. In Rust the natural equivalent is implementing `core::fmt::Write` over
//! the UART, which gives us `kprint!`/`kprintln!` with the full `{}` format
//! machinery. This is brought in early because every subsystem logs through it.

use core::fmt::{self, Write};
use crate::sync::SpinLock;

// Serializes formatted output across cores. IRQs are masked while held to
// avoid same-core re-entrant deadlock (e.g. a fault printing mid-print).
static PRINT_LOCK: SpinLock<()> = SpinLock::new(());

pub struct Uart;

impl Write for Uart {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        crate::uart::puts(s);
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    // Save + mask IRQs so an interrupt on this core can't re-enter and
    // deadlock on the lock; then serialize across cores.
    let daif: u64;
    unsafe { core::arch::asm!("mrs {}, daif", out(reg) daif) };
    unsafe { core::arch::asm!("msr daifset, #2") };
    {
        let _g = PRINT_LOCK.lock();
        let _ = Uart.write_fmt(args);
    }
    // Restore IRQ mask state (DAIF.I is bit 7).
    if daif & (1 << 7) == 0 {
        unsafe { core::arch::asm!("msr daifclr, #2") };
    }
}

#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => ($crate::print::_print(format_args!($($arg)*)));
}

#[macro_export]
macro_rules! kprintln {
    () => ($crate::print::_print(format_args!("\n")));
    ($($arg:tt)*) => ({
        // Fold the newline into a single locked _print so a full line is
        // emitted atomically (important once multiple cores print).
        $crate::print::_print(format_args!("{}\n", format_args!($($arg)*)));
    });
}
