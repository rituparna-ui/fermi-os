//! Kernel support library: low-level helpers shared across subsystems.

// These helpers form a stable API surface; some entry points are consumed by
// subsystems ported in later steps.
#![allow(dead_code)]

pub mod mmio;
pub mod sync;
pub mod uart;
