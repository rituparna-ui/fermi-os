//! Memory management: physical frame allocator (PMM), virtual memory (MMU),
//! and the kernel heap. Ported following the original commit progression.

#![allow(dead_code)]

pub mod pmm;
