//! Exception handling: vector table install, trap-frame ABI, ESR/DFSC decoding,
//! and the dispatch routine called from `exception_common` (vector.S).
//!
//! The trap-frame layout is a frozen ABI shared with `vector.S` (and later
//! `switch.S`): see docs/cref/00-PORT-PLAN.md §2.1. The C `trap_frame_t` is only
//! the first 40 bytes (regs + elr + spsr + esr + far); SP_EL0 lives at byte
//! offset 280 and is accessed by raw offset, not as a struct field.
//!
//! Dispatch arms for subsystems not yet ported (syscall, scheduler, timer, GIC)
//! are stubbed: they log and panic. They are filled in as those land.

#![allow(dead_code)]

pub mod gic;
pub mod timer;

use crate::kprintln;
use crate::panic::kernel_panic;
use crate::{mrs, msr};
use core::arch::global_asm;
use core::sync::atomic::{AtomicPtr, Ordering};

global_asm!(include_str!("vector.S"));

extern "C" {
    static vector_table: u8;
}

/// Trap frame saved by `exception_common`. `#[repr(C)]`; the field layout must
/// match the byte offsets used in vector.S. Note SP_EL0 (offset 280) and the
/// FP/SIMD save area are NOT fields here — they are accessed by raw offset.
#[repr(C)]
pub struct TrapFrame {
    pub regs: [u64; 31], // x0..x30        (offset 0)
    pub elr: u64,        // ELR_EL1        (offset 248)
    pub spsr: u64,       // SPSR_EL1       (offset 256)
    pub esr: u64,        // ESR_EL1        (offset 264)
    pub far: u64,        // FAR_EL1        (offset 272)
}

// Freeze the ABI: these offsets are baked into vector.S.
const _: () = assert!(core::mem::offset_of!(TrapFrame, regs) == 0);
const _: () = assert!(core::mem::offset_of!(TrapFrame, elr) == 248);
const _: () = assert!(core::mem::offset_of!(TrapFrame, spsr) == 256);
const _: () = assert!(core::mem::offset_of!(TrapFrame, esr) == 264);
const _: () = assert!(core::mem::offset_of!(TrapFrame, far) == 272);

/// Byte offset of SP_EL0 within the full 688-byte frame (raw access only).
pub const SP_EL0_OFFSET: usize = 280;
/// Full assembly frame size.
pub const FRAME_SIZE: usize = 688;

// Exception types (match the `mov x0, #type` constants in vector.S).
pub const EXCEPTION_SYNC: u64 = 0;
pub const EXCEPTION_IRQ: u64 = 1;
pub const EXCEPTION_FIQ: u64 = 2;
pub const EXCEPTION_SERROR: u64 = 3;

// ESR_EL1 Exception Class (bits [31:26]).
const ESR_EC_SHIFT: u64 = 26;
#[inline]
fn esr_ec(esr: u64) -> u64 {
    (esr >> ESR_EC_SHIFT) & 0x3F
}

pub const EC_UNKNOWN: u64 = 0x00;
pub const EC_WF_TRAPPED: u64 = 0x01;
pub const EC_SVC_AARCH64: u64 = 0x15;
pub const EC_HVC_AARCH64: u64 = 0x16;
pub const EC_SMC_AARCH64: u64 = 0x17;
pub const EC_INST_ABORT_LO: u64 = 0x20;
pub const EC_INST_ABORT_CUR: u64 = 0x21;
pub const EC_PC_ALIGN: u64 = 0x22;
pub const EC_DATA_ABORT_LO: u64 = 0x24;
pub const EC_DATA_ABORT_CUR: u64 = 0x25;
pub const EC_SP_ALIGN: u64 = 0x26;
pub const EC_FP_AARCH64: u64 = 0x2C;
pub const EC_SERROR: u64 = 0x2F;
pub const EC_BRK: u64 = 0x3C;

// ESR_EL1 ISS layout for data/instruction aborts.
#[inline]
pub fn esr_iss_dfsc(esr: u64) -> u8 {
    (esr & 0x3F) as u8
}
#[inline]
pub fn esr_iss_wnr(esr: u64) -> bool {
    (esr >> 6) & 1 != 0
}
#[inline]
pub fn esr_iss_cm(esr: u64) -> bool {
    (esr >> 8) & 1 != 0
}
#[inline]
pub fn esr_iss_ea(esr: u64) -> bool {
    (esr >> 9) & 1 != 0
}

fn exception_type_str(t: u64) -> &'static str {
    match t {
        EXCEPTION_SYNC => "Synchronous",
        EXCEPTION_IRQ => "IRQ",
        EXCEPTION_FIQ => "FIQ",
        EXCEPTION_SERROR => "SError",
        _ => "Unknown",
    }
}

fn esr_class_str(ec: u64) -> &'static str {
    match ec {
        EC_UNKNOWN => "Unknown reason",
        EC_WF_TRAPPED => "WFI/WFE trapped",
        EC_SVC_AARCH64 => "SVC (AArch64)",
        EC_HVC_AARCH64 => "HVC (AArch64)",
        EC_SMC_AARCH64 => "SMC (AArch64)",
        EC_INST_ABORT_LO => "Instruction abort (lower EL)",
        EC_INST_ABORT_CUR => "Instruction abort (current EL)",
        EC_PC_ALIGN => "PC alignment fault",
        EC_DATA_ABORT_LO => "Data abort (lower EL)",
        EC_DATA_ABORT_CUR => "Data abort (current EL)",
        EC_SP_ALIGN => "SP alignment fault",
        EC_FP_AARCH64 => "Floating point exception",
        EC_SERROR => "SError interrupt",
        EC_BRK => "BRK (debug breakpoint)",
        _ => "Unrecognized EC",
    }
}

/// DFSC (fault status) decode for aborts — ARM ARM (DDI 0487) Table D13-9.
pub fn dfsc_str(dfsc: u8) -> &'static str {
    match dfsc {
        0x00 => "Address size fault L0 / TTBR",
        0x01 => "Address size fault L1",
        0x02 => "Address size fault L2",
        0x03 => "Address size fault L3",
        0x04 => "Translation fault L0",
        0x05 => "Translation fault L1",
        0x06 => "Translation fault L2",
        0x07 => "Translation fault L3",
        0x09 => "Access flag fault L1",
        0x0a => "Access flag fault L2",
        0x0b => "Access flag fault L3",
        0x0d => "Permission fault L1",
        0x0e => "Permission fault L2",
        0x0f => "Permission fault L3",
        0x10 => "Synchronous external abort",
        0x14 => "Sync ext abort on TT walk L0",
        0x15 => "Sync ext abort on TT walk L1",
        0x16 => "Sync ext abort on TT walk L2",
        0x17 => "Sync ext abort on TT walk L3",
        0x21 => "Alignment fault",
        0x30 => "TLB conflict abort",
        _ => "Unknown DFSC",
    }
}

/// Classify a user FAR against the fixed EL0 address regions for one-glance
/// fault diagnosis.
fn va_classify_user(far: u64) -> &'static str {
    use crate::mm::consts::{USER_STACK_TOP, USER_TEXT_BASE};
    let stack_pages = crate::sched::USER_STACK_PAGES_MAX; // active growth window upper bound
    let _ = stack_pages;
    if far >= 0xFFFF_0000_0000_0000 {
        return "kernel-half VA (kernel-pointer leak?)";
    }
    if far < 0x1000 {
        return "NULL-page (nullptr deref)";
    }
    let stack_lo = USER_STACK_TOP - 4 * 0x1000; // initial 16 KiB stack
    if far >= USER_TEXT_BASE && far < stack_lo {
        return "user code / data region";
    }
    if far >= stack_lo && far < USER_STACK_TOP {
        return "user stack (active)";
    }
    if far >= stack_lo - 0x1000 && far < stack_lo {
        return "just below user stack — STACK OVERFLOW likely";
    }
    if far >= USER_STACK_TOP {
        return "above user range — wild pointer";
    }
    "user lower-half (unmapped)"
}

/// Compact decoded user-fault dump (data + instruction aborts from EL0).
fn dump_user_abort(what: &str, t: *mut crate::sched::Task, frame: &TrapFrame) {
    let ttbr0 = mrs!("ttbr0_el1");
    let asid = (ttbr0 >> 48) as u16;
    let dfsc = esr_iss_dfsc(frame.esr);
    let (pid, name) = unsafe { ((*t).pid, crate::sched::task_name(t)) };

    kprintln!("[FAULT] {} in task {} '{}' ASID={}", what, pid, name, asid);
    kprintln!(
        "  ELR={:#x}  FAR={:#x}  ESR={:#x}  SPSR={:#x}",
        frame.elr,
        frame.far,
        frame.esr,
        frame.spsr
    );
    kprintln!(
        "  cause: {} (DFSC={:#x}){}{}{}",
        dfsc_str(dfsc),
        dfsc,
        if esr_iss_wnr(frame.esr) { "  write" } else { "  read" },
        if esr_iss_cm(frame.esr) { "  cache-maint" } else { "" },
        if esr_iss_ea(frame.esr) { "  external-abort" } else { "" }
    );
    kprintln!("  FAR region: {}", va_classify_user(frame.far));
    kprintln!("  -> killing task");
}

fn dump_trap_frame(t: u64, frame: &TrapFrame) {
    kprintln!("");
    kprintln!("========== EXCEPTION ==========");
    kprintln!("  Type : {}", exception_type_str(t));
    let ec = esr_ec(frame.esr);
    kprintln!("  Class: {} (EC={:#x})", esr_class_str(ec), ec);
    kprintln!("  ESR_EL1 : {:#x}", frame.esr);
    kprintln!("  ELR_EL1 : {:#x}", frame.elr);
    kprintln!("  FAR_EL1 : {:#x}", frame.far);
    kprintln!("  SPSR_EL1 : {:#x}", frame.spsr);
    kprintln!("  Registers:");
    for (i, r) in frame.regs.iter().enumerate() {
        kprintln!("    x{} = {:#x}", i, r);
    }
    kprintln!("===============================");
}

/// Hook invoked at the end of IRQ handling to let the scheduler preempt. Set by
/// the scheduler when it starts; a null pointer means "no scheduler yet".
static SCHEDULE_HOOK: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

/// Register the scheduler's preemption entry point.
pub fn set_schedule_hook(hook: extern "C" fn()) {
    SCHEDULE_HOOK.store(hook as *mut (), Ordering::SeqCst);
}

#[inline]
fn run_schedule_hook() {
    let p = SCHEDULE_HOOK.load(Ordering::SeqCst);
    if !p.is_null() {
        // SAFETY: only ever set to a valid `extern "C" fn()` by set_schedule_hook.
        let f: extern "C" fn() = unsafe { core::mem::transmute(p) };
        f();
    }
}

/// Called from `exception_common` (vector.S) with `type` and the trap frame.
#[no_mangle]
pub extern "C" fn exception_dispatch(exc_type: u64, frame: &mut TrapFrame) {
    let ec = esr_ec(frame.esr);

    match exc_type {
        EXCEPTION_SYNC => match ec {
            EC_SVC_AARCH64 => {
                crate::syscall::dispatch(frame);
            }
            EC_DATA_ABORT_CUR => {
                dump_trap_frame(exc_type, frame);
                kernel_panic("Data abort (kernel)");
            }
            EC_DATA_ABORT_LO => {
                // Translation faults in the user stack-growth zone => demand
                // page and resume; everything else kills the offending task.
                let t = crate::sched::current();
                let dfsc = esr_iss_dfsc(frame.esr);
                if (dfsc == 0x05 || dfsc == 0x06 || dfsc == 0x07)
                    && crate::sched::try_grow_stack(t, frame.far)
                {
                    // resume the faulting instruction
                } else {
                    dump_user_abort("data abort", t, frame);
                    crate::sched::task_exit();
                }
            }
            EC_INST_ABORT_CUR => {
                dump_trap_frame(exc_type, frame);
                kernel_panic("Instruction abort (kernel)");
            }
            EC_INST_ABORT_LO => {
                let t = crate::sched::current();
                dump_user_abort("instruction abort", t, frame);
                crate::sched::task_exit();
            }
            EC_BRK => {
                kprintln!("[EXCEPTION] Breakpoint hit");
                dump_trap_frame(exc_type, frame);
                frame.elr += 4; // skip the BRK to avoid an infinite loop
            }
            _ => {
                dump_trap_frame(exc_type, frame);
                kernel_panic("Unhandled synchronous exception");
            }
        },

        EXCEPTION_IRQ => {
            let intid = gic::ack_irq();
            if intid == gic::GIC_INTID_NO_PENDING {
                return;
            }
            gic::count_irq(intid as u32);

            if intid as u32 == timer::TIMER_PPI_INTID {
                timer::handle_irq();
            } else {
                kprintln!("[IRQ] INTID {} (not implemented)", intid);
            }

            gic::end_irq(intid);

            // Schedule after EOI so the GIC can deliver future IRQs. The
            // scheduler registers this hook when it starts; until then it's a
            // no-op (the kernel just returns to whatever it preempted).
            run_schedule_hook();
        }

        EXCEPTION_FIQ => {
            dump_trap_frame(exc_type, frame);
            kernel_panic("Unexpected FIQ");
        }

        EXCEPTION_SERROR => {
            dump_trap_frame(exc_type, frame);
            kernel_panic("SError (asynchronous abort)");
        }

        _ => {
            dump_trap_frame(exc_type, frame);
            kernel_panic("Unknown exception type");
        }
    }
}

/// Install the vector table (pre-MMU; `&vector_table` is PC-relative → physical).
pub fn init() {
    kprintln!("[EXCEPTION] Installing vector table (physical)");
    let vbar = core::ptr::addr_of!(vector_table) as u64;
    kprintln!("[EXCEPTION] VBAR_EL1 = {:#x}", vbar);
    unsafe {
        msr!("vbar_el1", vbar);
        core::arch::asm!("isb");
    }
    kprintln!("[EXCEPTION] Vector table installed!");
}

/// Re-point VBAR_EL1 at the upper-half vector table after the MMU jump.
pub fn init_upper() {
    kprintln!("[EXCEPTION] Relocating vector table to upper half");
    let vbar = core::ptr::addr_of!(vector_table) as u64;
    kprintln!("[EXCEPTION] VBAR_EL1 = {:#x}", vbar);
    unsafe {
        msr!("vbar_el1", vbar);
        core::arch::asm!("isb");
    }
    kprintln!("[EXCEPTION] Vector table relocated!");
}

/// Read the current VBAR_EL1 (for verification).
pub fn vbar() -> u64 {
    mrs!("vbar_el1")
}
