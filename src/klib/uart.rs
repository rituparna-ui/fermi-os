//! PL011 UART driver (QEMU `virt` serial port at 0x0900_0000).
//!
//! Provides byte/string output, line helpers, hex/decimal/binary formatting,
//! and — via `core::fmt::Write` — full `write!`/`println!` formatting. The C
//! original exposed a custom `uart_printf` with `%s %d %u %x %p %b %c %%`;
//! Rust's formatting machinery supersedes it, and the `kprintln!` macros below
//! give the same ergonomics.

use crate::klib::mmio;
use core::fmt::{self, Write};

const UART_BASE: usize = 0x0900_0000;
const UART_DR: usize = UART_BASE + 0x00; // data register
const UART_FR: usize = UART_BASE + 0x18; // flag register
const UART_IBRD: usize = UART_BASE + 0x24; // integer baud rate divisor
const UART_FBRD: usize = UART_BASE + 0x28; // fractional baud rate divisor
const UART_LCRH: usize = UART_BASE + 0x2C; // line control
const UART_CR: usize = UART_BASE + 0x30; // control register
const UART_ICR: usize = UART_BASE + 0x44; // interrupt clear register

const FR_TXFF: u32 = 1 << 5; // transmit FIFO full
const FR_RXFE: u32 = 1 << 4; // receive FIFO empty

/// Zero-sized handle to the PL011. All state lives in the device registers.
pub struct Uart;

impl Uart {
    /// Configure the PL011: 115200 8N1, FIFOs enabled, RX+TX on.
    pub fn init(&self) {
        // Disable the UART before reconfiguring.
        mmio::write32(UART_CR, 0x0000_0000);
        // Clear all pending interrupts.
        mmio::write32(UART_ICR, 0x7FF);

        // Baud rate divisor for 115200 @ 24 MHz reference clock:
        //   divisor = 24_000_000 / (16 * 115200) = 13.0208…
        //   IBRD = 13, FBRD = round(0.0208 * 64) = 2
        mmio::write32(UART_IBRD, 13);
        mmio::write32(UART_FBRD, 2);

        // 8-bit words, 1 stop bit, no parity, FIFOs enabled.
        mmio::write32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

        // Enable UART, TX and RX.
        mmio::write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
    }

    /// Write a single byte, blocking while the TX FIFO is full.
    pub fn putc(&self, c: u8) {
        while mmio::read32(UART_FR) & FR_TXFF != 0 {}
        mmio::write32(UART_DR, c as u32);
    }

    /// Read a single byte, blocking while the RX FIFO is empty.
    pub fn getc(&self) -> u8 {
        while mmio::read32(UART_FR) & FR_RXFE != 0 {}
        mmio::read32(UART_DR) as u8
    }

    /// Write a string.
    pub fn puts(&self, s: &str) {
        for b in s.bytes() {
            self.putc(b);
        }
    }

    /// Write a string followed by a newline.
    pub fn println(&self, s: &str) {
        self.puts(s);
        self.putc(b'\n');
    }

    /// Write an `[ERROR!]:`-prefixed line.
    pub fn errorln(&self, s: &str) {
        self.puts("[ERROR!]: ");
        self.puts(s);
        self.putc(b'\n');
    }

    /// Print `value` as `0x`-prefixed hexadecimal (leading zeros trimmed).
    pub fn puthex(&self, value: u64) {
        self.puts("0x");
        let mut started = false;
        let mut shift: i32 = 60;
        while shift >= 0 {
            let nibble = ((value >> shift) & 0xF) as u8;
            if nibble != 0 || started || shift == 0 {
                self.putc(if nibble < 10 {
                    b'0' + nibble
                } else {
                    b'A' + nibble - 10
                });
                started = true;
            }
            shift -= 4;
        }
    }

    /// Print `value` in decimal.
    pub fn putdec(&self, mut value: u64) {
        if value == 0 {
            self.putc(b'0');
            return;
        }
        let mut buf = [0u8; 20];
        let mut i = 0;
        while value != 0 {
            buf[i] = b'0' + (value % 10) as u8;
            value /= 10;
            i += 1;
        }
        while i != 0 {
            i -= 1;
            self.putc(buf[i]);
        }
    }

    /// Print `value` as `0b`-prefixed binary (leading zeros trimmed).
    pub fn putbin(&self, value: u64) {
        self.puts("0b");
        let mut started = false;
        let mut shift: i32 = 63;
        while shift >= 0 {
            let bit = ((value >> shift) & 1) as u8;
            if bit != 0 || started || shift == 0 {
                self.putc(b'0' + bit);
                started = true;
            }
            shift -= 1;
        }
    }
}

/// `core::fmt::Write` lets the UART back `write!`/`writeln!` and the `kprint!`
/// family of macros.
impl Write for Uart {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.puts(s);
        Ok(())
    }
}

/// Initialize the global UART. Call once early in boot.
pub fn init() {
    Uart.init();
    Uart.println("UART Initialized !");
}

/// Internal: used by the `kprint!` macros.
#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    // The UART is stateless (a ZST over fixed MMIO), so a fresh handle is fine.
    let _ = Uart.write_fmt(args);
}

/// `print!`-style formatted output over the UART (no trailing newline).
#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => ($crate::klib::uart::_print(format_args!($($arg)*)));
}

/// `println!`-style formatted output over the UART.
#[macro_export]
macro_rules! kprintln {
    () => ($crate::kprint!("\n"));
    ($($arg:tt)*) => ($crate::kprint!("{}\n", format_args!($($arg)*)));
}
