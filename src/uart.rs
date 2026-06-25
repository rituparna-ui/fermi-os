//! PL011 UART driver (QEMU virt, UART0 @ 0x0900_0000).
//!
//! Direct port of the original `src/lib/uart/uart.c`.

use crate::mmio;

pub const UART_BASE: usize = 0x0900_0000;
const UART_DR: usize = UART_BASE + 0x00;
const UART_FR: usize = UART_BASE + 0x18;
const UART_IBRD: usize = UART_BASE + 0x24;
const UART_FBRD: usize = UART_BASE + 0x28;
const UART_LCRH: usize = UART_BASE + 0x2C;
const UART_CR: usize = UART_BASE + 0x30;
const UART_ICR: usize = UART_BASE + 0x44;

const FR_TXFF: u32 = 1 << 5; // transmit FIFO full
const FR_RXFE: u32 = 1 << 4; // receive FIFO empty

/// Initialise the PL011: 115200 8N1, FIFOs enabled, TX+RX enabled.
pub fn init() {
    // Disable UART.
    mmio::write32(UART_CR, 0x0000_0000);
    // Clear pending interrupts.
    mmio::write32(UART_ICR, 0x7FF);
    // Baud rate divisor for 24 MHz UARTCLK @ 115200: int=13, frac=2.
    mmio::write32(UART_IBRD, 13);
    mmio::write32(UART_FBRD, 2);
    // 8-bit word, 1 stop bit, no parity, FIFO enabled.
    mmio::write32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));
    // Enable UART, RX, TX.
    mmio::write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));

    println("UART Initialized !");
}

pub fn putc(c: u8) {
    while mmio::read32(UART_FR) & FR_TXFF != 0 {}
    mmio::write32(UART_DR, c as u32);
}

pub fn getc() -> u8 {
    while mmio::read32(UART_FR) & FR_RXFE != 0 {}
    mmio::read32(UART_DR) as u8
}

pub fn puts(s: &str) {
    for b in s.bytes() {
        putc(b);
    }
}

pub fn println(s: &str) {
    puts(s);
    putc(b'\n');
}

pub fn errorln(s: &str) {
    puts("[ERROR!]: ");
    puts(s);
    putc(b'\n');
}

pub fn puthex(value: u64) {
    puts("0x");
    let mut started = false;
    let mut i: i32 = 60;
    while i >= 0 {
        let nibble = ((value >> i) & 0xF) as u8;
        if nibble != 0 || started || i == 0 {
            putc(if nibble < 10 {
                b'0' + nibble
            } else {
                b'A' + nibble - 10
            });
            started = true;
        }
        i -= 4;
    }
}

pub fn putdec(mut value: u64) {
    if value == 0 {
        putc(b'0');
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
        putc(buf[i]);
    }
}

pub fn putbin(value: u64) {
    puts("0b");
    let mut started = false;
    let mut i: i32 = 63;
    while i >= 0 {
        let bit = ((value >> i) & 1) as u8;
        if bit != 0 || started || i == 0 {
            putc(b'0' + bit);
            started = true;
        }
        i -= 1;
    }
}

/// Pre-MMU log helper: `prefix` then a decimal value then newline.
/// Uses only aligned byte stores (no core::fmt), so it is safe on Device
/// memory before the MMU is enabled.
pub fn log_dec(prefix: &str, v: u64) {
    puts(prefix);
    putdec(v);
    putc(b'\n');
}

/// Pre-MMU log helper: `prefix` then a hex value then newline.
pub fn log_hex(prefix: &str, v: u64) {
    puts(prefix);
    puthex(v);
    putc(b'\n');
}
