//! Architecture-specific code (aarch64 / ARMv8-A).
//!
//! Assembly lives in sibling `.S` files and is pulled into the build via
//! `global_asm!(include_str!(...))` from the crate root.
