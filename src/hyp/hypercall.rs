//! Fermi hypervisor call ABI — shared by the guest (EL1) and the hypervisor
//! (EL2 dispatch).
//!
//! Calling convention (SMCCC-like): `x0` = function ID, `x1..x3` = arguments,
//! then `hvc #0`. On return `x0` = result; `x1..x3` are preserved by the
//! hypervisor. The function ID lives in `x0` (not the HVC immediate) so guests
//! can dispatch dynamically without self-modifying code.

#![allow(dead_code)]

pub const HVC_VERSION: u64 = 0; // () -> ABI version
pub const HVC_PUTC: u64 = 1; // (char in x1) -> 0 ; paravirt console putc
pub const HVC_PING: u64 = 2; // (val in x1) -> val + 1 ; liveness / echo
pub const HVC_VM_INFO: u64 = 3; // () -> hypercalls serviced for this vCPU
pub const HVC_YIELD: u64 = 4; // () -> 0 ; cooperative yield (stub until M5)

pub const HYP_ABI_VERSION: u64 = 0x0001_0000; // 1.0
pub const HVC_ERR_BADCALL: u64 = u64::MAX; // unknown function ID

/// Guest-side hypercall trampoline. Issues `hvc #0` with the SMCCC-style
/// register layout and returns the hypervisor's `x0` result.
///
/// # Safety
/// Must be called from EL1 with a hypervisor present beneath us (i.e. the image
/// was entered at EL2 and `hyp_init` installed `VBAR_EL2`). Issuing `HVC` when
/// EL2 is absent is UNDEFINED and traps to EL1 — callers gate on
/// [`crate::hyp::booted_via_el2`].
#[inline]
pub unsafe fn hvc_call(fn_id: u64, a1: u64, a2: u64, a3: u64) -> u64 {
    let mut x0 = fn_id;
    // SAFETY: caller guarantees EL2 is present; clobbers nothing beyond x0..x3
    // and memory (the hypervisor may emit console output).
    unsafe {
        core::arch::asm!(
            "hvc #0",
            inout("x0") x0,
            in("x1") a1,
            in("x2") a2,
            in("x3") a3,
            options(nostack),
        );
    }
    x0
}
