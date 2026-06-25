//! Thin wrappers around aarch64 system-register access.
//!
//! These are the Rust equivalent of the C `MRS`/`MSR` macros. They must be
//! `inline` and use `asm!` directly — there is no way to read a sysreg in safe
//! Rust. Reading is side-effect-free; writing can reconfigure the core, so the
//! write side is `unsafe`.

/// Read a 64-bit system register by name, e.g. `mrs!("CurrentEL")`.
#[macro_export]
macro_rules! mrs {
    ($reg:literal) => {{
        let value: u64;
        // SAFETY: reading a system register has no memory side effects. This is
        // wrapped in its own unsafe block so the macro is usable from safe code;
        // `#[allow(unused_unsafe)]` keeps it quiet when expanded inside an
        // existing unsafe block.
        #[allow(unused_unsafe)]
        unsafe {
            core::arch::asm!(
                concat!("mrs {0}, ", $reg),
                out(reg) value,
                options(nomem, nostack, preserves_flags),
            );
        }
        value
    }};
}

/// Write a 64-bit value to a system register by name, e.g.
/// `msr!("pmcr_el0", value)`. Unsafe because it can reconfigure the CPU.
#[macro_export]
macro_rules! msr {
    ($reg:literal, $val:expr) => {{
        let value: u64 = $val;
        core::arch::asm!(
            concat!("msr ", $reg, ", {0}"),
            in(reg) value,
            options(nomem, nostack, preserves_flags),
        );
    }};
}
