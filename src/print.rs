//! Formatted kernel output.
//!
//! The original C kernel grew a variadic `uart_printf` a few commits after the
//! PMM. In Rust the natural equivalent is implementing `core::fmt::Write` over
//! the UART, which gives us `kprint!`/`kprintln!` with the full `{}` format
//! machinery. This is brought in early because every subsystem logs through it.

use core::fmt::{self, Write};

pub struct Uart;

impl Write for Uart {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        crate::uart::puts(s);
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    // Single-core; UART FIFO writes are inherently serialized by the busy-wait.
    let _ = Uart.write_fmt(args);
}

#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => ($crate::print::_print(format_args!($($arg)*)));
}

#[macro_export]
macro_rules! kprintln {
    () => ($crate::uart::putc(b'\n'));
    ($($arg:tt)*) => ({
        $crate::print::_print(format_args!($($arg)*));
        $crate::uart::putc(b'\n');
    });
}
