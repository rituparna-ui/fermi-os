//! Exception handling: trap frame, ESR/DFSC decode, dispatch.
//!
//! Port of `src/exception/exception.c` + `vector.S`. The dispatch starts with
//! fault diagnosis + panic; SVC/IRQ/timer/scheduler integration is wired in as
//! those subsystems are ported (mirroring how the original grew exception.c).

use crate::kprintln;
use crate::mm::mmu::{USER_STACK_PAGES, USER_STACK_TOP, USER_TEXT_BASE};
use crate::panic::kernel_panic;
use crate::uart;
use core::arch::global_asm;

global_asm!(include_str!("exception/vector.S"));

pub mod gic;
pub mod timer;

extern "C" {
    static vector_table: u8;
}

use crate::sync::Racy;

#[derive(Default)]
struct TrapStats {
    sync: u64,
    irq: u64,
    fiq: u64,
    serror: u64,
    svc: u64,
    data_abort: u64,
    inst_abort: u64,
    brk: u64,
}
static TRAPS: Racy<TrapStats> = Racy::new(TrapStats {
    sync: 0, irq: 0, fiq: 0, serror: 0, svc: 0, data_abort: 0, inst_abort: 0, brk: 0,
});

/// Render a /proc-style trap-count summary.
pub fn render_stats() -> alloc::string::String {
    use core::fmt::Write;
    let t = unsafe { TRAPS.get() };
    let mut s = alloc::string::String::new();
    let _ = writeln!(s, "sync        : {}", t.sync);
    let _ = writeln!(s, "  svc       : {}", t.svc);
    let _ = writeln!(s, "  data_abort: {}", t.data_abort);
    let _ = writeln!(s, "  inst_abort: {}", t.inst_abort);
    let _ = writeln!(s, "  brk       : {}", t.brk);
    let _ = writeln!(s, "irq         : {}", t.irq);
    let _ = writeln!(s, "fiq         : {}", t.fiq);
    let _ = writeln!(s, "serror      : {}", t.serror);
    s
}

/// Trap frame — must match the layout written by `vector.S`.
#[repr(C)]
pub struct TrapFrame {
    pub regs: [u64; 31],
    pub elr: u64,
    pub spsr: u64,
    pub esr: u64,
    pub far: u64,
}

// Exception types (x0 from the vector entry).
pub const EXCEPTION_SYNC: u64 = 0;
pub const EXCEPTION_IRQ: u64 = 1;
pub const EXCEPTION_FIQ: u64 = 2;
pub const EXCEPTION_SERROR: u64 = 3;

// ESR_EL1 Exception Class (bits [31:26]).
#[inline(always)]
pub fn esr_ec(esr: u64) -> u64 {
    (esr >> 26) & 0x3F
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

// ESR ISS for data/instruction aborts.
#[inline(always)]
pub fn esr_iss_dfsc(esr: u64) -> u8 {
    (esr & 0x3F) as u8
}
#[inline(always)]
pub fn esr_iss_wnr(esr: u64) -> u64 {
    (esr >> 6) & 1
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
        0x21 => "Alignment fault",
        0x30 => "TLB conflict abort",
        _ => "Unknown DFSC",
    }
}

/// Classify a user FAR against the fixed EL0 address-space regions.
pub fn va_classify_user(far: u64) -> &'static str {
    if far >= 0xFFFF_0000_0000_0000 {
        return "kernel-half VA (kernel-pointer leak?)";
    }
    if far < 0x1000 {
        return "NULL-page (nullptr deref)";
    }
    let stack_lo = USER_STACK_TOP - USER_STACK_PAGES * 0x1000;
    if far >= USER_TEXT_BASE && far < stack_lo {
        return "user code / data region";
    }
    if far >= stack_lo && far < USER_STACK_TOP {
        return "user stack (active)";
    }
    if far >= stack_lo - 0x1000 && far < stack_lo {
        return "just below user stack - STACK OVERFLOW likely";
    }
    if far >= USER_STACK_TOP {
        return "above user range - wild pointer";
    }
    "user lower-half (unmapped)"
}

fn dump_trap_frame(type_: u64, frame: &TrapFrame) {
    kprintln!("");
    kprintln!("========== EXCEPTION ==========");
    kprintln!("  Type : {}", exception_type_str(type_));
    let ec = esr_ec(frame.esr);
    kprintln!("  Class: {} (EC={:#x})", esr_class_str(ec), ec);
    kprintln!("  ESR_EL1 : {:#x}", frame.esr);
    kprintln!("  ELR_EL1 : {:#x}", frame.elr);
    kprintln!("  FAR_EL1 : {:#x}", frame.far);
    kprintln!("  SPSR_EL1: {:#x}", frame.spsr);
    kprintln!("  Registers:");
    for i in 0..31 {
        kprintln!("    x{} = {:#x}", i, frame.regs[i]);
    }
    kprintln!("===============================");
}

#[no_mangle]
pub extern "C" fn exception_dispatch(type_: u64, frame: *mut TrapFrame) {
    let frame = unsafe { &mut *frame };
    let ec = esr_ec(frame.esr);
    {
        let t = unsafe { TRAPS.get() };
        match type_ {
            EXCEPTION_SYNC => {
                t.sync += 1;
                match ec {
                    EC_SVC_AARCH64 => t.svc += 1,
                    EC_DATA_ABORT_LO | EC_DATA_ABORT_CUR => t.data_abort += 1,
                    EC_INST_ABORT_LO | EC_INST_ABORT_CUR => t.inst_abort += 1,
                    EC_BRK => t.brk += 1,
                    _ => {}
                }
            }
            EXCEPTION_IRQ => t.irq += 1,
            EXCEPTION_FIQ => t.fiq += 1,
            EXCEPTION_SERROR => t.serror += 1,
            _ => {}
        }
    }

    match type_ {
        EXCEPTION_SYNC => match ec {
            EC_SVC_AARCH64 => {
                crate::syscall::syscall_dispatch(frame);
            }
            EC_DATA_ABORT_CUR => {
                dump_trap_frame(type_, frame);
                kernel_panic("Data abort (kernel)");
            }
            EC_DATA_ABORT_LO => {
                let dfsc = esr_iss_dfsc(frame.esr);
                // Translation fault in the stack-growth zone -> demand-page it.
                if (dfsc == 0x05 || dfsc == 0x06 || dfsc == 0x07)
                    && crate::sched::try_grow_stack(frame.far)
                {
                    return; // eret resumes the faulting instruction
                }
                kprintln!(
                    "[FAULT] user data abort: {} FAR={:#x} region={}",
                    dfsc_str(dfsc),
                    frame.far,
                    va_classify_user(frame.far)
                );
                kprintln!("  -> killing task");
                crate::sched::task_exit();
            }
            EC_INST_ABORT_CUR => {
                dump_trap_frame(type_, frame);
                kernel_panic("Instruction abort (kernel)");
            }
            EC_INST_ABORT_LO => {
                kprintln!(
                    "[FAULT] user instruction abort FAR={:#x} region={}",
                    frame.far,
                    va_classify_user(frame.far)
                );
                kprintln!("  -> killing task");
                crate::sched::task_exit();
            }
            EC_BRK => {
                kprintln!("[EXCEPTION] Breakpoint hit");
                dump_trap_frame(type_, frame);
                frame.elr += 4; // skip the BRK to avoid re-faulting
            }
            _ => {
                dump_trap_frame(type_, frame);
                kernel_panic("Unhandled synchronous exception");
            }
        },
        EXCEPTION_IRQ => {
            let intid = gic::ack_irq();
            if intid == gic::GIC_INTID_NO_PENDING {
                return;
            }
            let core1 = crate::smp::is_secondary();
            if !core1 {
                gic::count_irq(intid as u32);
            }
            if intid as u32 == timer::TIMER_PPI_INTID {
                if core1 {
                    crate::smp::c1_timer_tick();
                } else {
                    timer::handle_irq();
                }
            } else if intid as u32 == crate::virtio::net::irq_intid() {
                crate::virtio::net::handle_irq();
            } else if intid as u32 == crate::virtio::blk::irq_intid() {
                crate::virtio::blk::handle_irq();
            } else {
                kprintln!("[IRQ] INTID {} (not implemented)", intid);
            }
            gic::end_irq(intid);
            // Per-core preemption after EOI.
            if core1 {
                crate::smp::c1_preempt();
            } else {
                crate::schedule_hook();
            }
        }
        EXCEPTION_FIQ => {
            dump_trap_frame(type_, frame);
            kernel_panic("Unexpected FIQ");
        }
        EXCEPTION_SERROR => {
            dump_trap_frame(type_, frame);
            kernel_panic("SError (asynchronous abort)");
        }
        _ => {
            dump_trap_frame(type_, frame);
            kernel_panic("Unknown exception type");
        }
    }
}

/// Set VBAR_EL1 to the vector table on the current core (no logging).
pub fn set_vbar_current() {
    let vbar = unsafe { &vector_table as *const u8 as u64 };
    crate::msr!(vbar_el1, vbar);
    unsafe { core::arch::asm!("isb") };
}

/// Install the vector table into VBAR_EL1.
pub fn init() {
    uart::println("[EXCEPTION] Installing vector table");
    let vbar = unsafe { &vector_table as *const u8 as u64 };
    crate::msr!(vbar_el1, vbar);
    unsafe { core::arch::asm!("isb") };
    kprintln!("[EXCEPTION] VBAR_EL1 = {:#x}", vbar);
    uart::println("[EXCEPTION] Vector table installed!");
}
