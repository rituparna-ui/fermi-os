//! CPU helpers: system-register access and current exception level.
//!
//! Ports the exception-level helpers from the original `src/lib/utils/utils.c`.
//! Richer CPU identification / PMU support is added when the corresponding
//! later commit is reached.

/// Read a system register into a u64.
#[macro_export]
macro_rules! mrs {
    ($reg:tt) => {{
        let v: u64;
        unsafe { core::arch::asm!(concat!("mrs {x}, ", stringify!($reg)), x = out(reg) v) };
        v
    }};
}

/// Write a u64 to a system register.
#[macro_export]
macro_rules! msr {
    ($reg:tt, $val:expr) => {{
        let v: u64 = $val;
        unsafe { core::arch::asm!(concat!("msr ", stringify!($reg), ", {x}"), x = in(reg) v) };
    }};
}

/// Current exception level (0..=3), from `CurrentEL[3:2]`.
pub fn current_el() -> u8 {
    let cur: u64 = mrs!(CurrentEL);
    ((cur >> 2) & 0b11) as u8
}

pub fn el_name(el: u8) -> &'static str {
    match el {
        0 => "User Space",
        1 => "Kernel Space",
        2 => "Hyper Space",
        3 => "Secure Monitor/Firmware",
        _ => "Invalid Exception Level",
    }
}

pub fn print_current_el() {
    crate::uart::puts("Current Exception Level: ");
    crate::uart::println(el_name(current_el()));
}
