//! Fermi OS — a bare-metal aarch64 (ARMv8-A) kernel in pure Rust + assembly.
//!
//! Targets QEMU's `virt` machine with a Cortex-A72. This is a from-scratch
//! Rust re-implementation of the original C kernel, built feature by feature.

#![no_std]
#![no_main]

#[macro_use]
mod klib;
#[macro_use]
mod arch;
mod mm;
mod panic;

use core::arch::global_asm;

// Pull in the assembly entry point. LLVM's integrated assembler handles the
// GNU-syntax `.S` file, so no external `as`/binutils is required.
global_asm!(include_str!("arch/boot.S"));

/// Early init — runs physically (PC == PA) with the MMU off, called from
/// `_start`. Brings up the UART, physical memory manager, and the MMU. After it
/// returns, `boot.S` switches to the upper-half stack and branches to `kmain`.
///
/// All logging here uses the UART's aligned helpers: RAM is Device memory until
/// the MMU maps it Normal, so `core::fmt` would fault.
#[no_mangle]
pub extern "C" fn early_init() {
    // Rust uses SIMD (incl. in core::fmt); enable it before anything formats.
    arch::cpu::enable_fp_simd();

    // IMPORTANT: everything before `mmu::init` runs at the physical PC with the
    // MMU off, so it must be position-independent. Code (PC-relative `adr`) and
    // string literals are fine, but Rust constructs that materialize *absolute*
    // VA pointers — `match`-on-enum returning `&str`, static reference arrays,
    // trait-object vtables — load upper-half addresses that aren't mapped yet
    // and fault. Keep the pre-MMU path to literal logging + computed pointers
    // only. Once the MMU is on, both halves are mapped and such data resolves.
    klib::uart::init();
    klib::uart::Uart.println("Fermi OS (Rust) - Booting Up...");

    // Physical memory manager (pre-MMU, identity mapped).
    mm::pmm::init(mm::pmm::MEM_START, mm::pmm::MEM_SIZE);
    mm::pmm::print_info();

    // Build page tables and enable the MMU; keep the L1 handle for the tests.
    let l1_phys = mm::mmu::init();

    // MMU is on now — absolute-VA data resolves via TTBR1 even at physical PC.
    arch::cpu::print_current_el();
    // Self-tests run while TTBR0 is still the boot identity table.
    mm::mmu::run_tests(l1_phys);

    klib::uart::Uart.println("[BOOT] MMU Enabled. Jumping to Upper Half");
}

/// Upper-half kernel entry, branched to from `boot.S` after the MMU is enabled
/// and SP has been switched to the upper-half stack. Never returns.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    // Route all device MMIO through the TTBR1 upper half from here on.
    klib::mmio::switch_to_upper(mm::consts::KERNEL_VA_OFFSET as usize);

    // Relocate the PMM bitmap pointer to its upper-half virtual address.
    mm::pmm::relocate_upper();

    // Now in the upper half with RAM mapped Normal: core::fmt is safe.
    kprintln!("[KERNEL] kmain address: {:#x}", kmain as usize as u64);
    let sp: u64;
    unsafe { core::arch::asm!("mov {}, sp", out(reg) sp) };
    kprintln!("[KERNEL] Stack Pointer: {:#x}", sp);

    kprintln!("[KERNEL] Ready! Entering idle loop.");
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}
