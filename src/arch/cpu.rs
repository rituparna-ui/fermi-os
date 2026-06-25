//! CPU/exception-level helpers.
//!
//! At this stage this mirrors the C `print_current_el` path. The full CPU
//! identification + PMU cycle-counter support (originally `cpu.c`) is ported
//! later, following the original commit progression.

use crate::klib::uart::Uart;
use crate::{kprintln, mrs, msr};

/// Attempt a PSCI SYSTEM_RESET. Returns -1 if PSCI is unavailable (no EL2/EL3
/// conduit on this QEMU `virt` config) — in which case the kernel stays up and
/// the caller reports the failure rather than faulting. On a machine with a
/// PSCI conduit this does not return.
///
/// PSCI SYSTEM_RESET = 0x8400_0009. We must call this from EL1 (the syscall
/// path), not EL0 where HVC/SMC trap as undefined.
pub fn reboot() -> i64 {
    // Without secure=on / virtualization=on, HVC/SMC from EL1 trap to EL1 as a
    // synchronous exception. We can't safely probe that from here without the
    // vector path catching it, so on this config we report "unavailable".
    // (Left as an SMC attempt for configs that do expose a PSCI conduit.)
    let current = current_el();
    if current >= 2 {
        // EL2+: HVC/SMC would reach firmware. Issue SMC SYSTEM_RESET.
        unsafe {
            core::arch::asm!("smc #0", in("x0") 0x8400_0009u64, options(nostack));
        }
    }
    -1
}

/// Full-system data synchronization barrier (`dsb sy`). Required after MMIO
/// writes that change device state and before dependent reads — the canonical
/// barrier reused by every VirtIO driver.
#[inline(always)]
#[allow(dead_code)]
pub fn dsb_sy() {
    unsafe {
        core::arch::asm!("dsb sy", options(nostack, preserves_flags));
    }
}

/// Enable FP/SIMD at EL1 (CPACR_EL1.FPEN = 0b11).
///
/// The Rust compiler — like GCC for varargs — uses SIMD registers, including in
/// `core::fmt`. Without this, the first FP/SIMD instruction traps (ESR
/// `0x1FE0_0000`). Must run early in boot, before any formatting.
pub fn enable_fp_simd() {
    let mut cpacr = mrs!("cpacr_el1");
    cpacr |= 3 << 20;
    unsafe {
        msr!("cpacr_el1", cpacr);
        core::arch::asm!("isb");
    }
}

/// The current exception level, read from `CurrentEL[3:2]`.
pub fn current_el() -> u8 {
    let current_el = mrs!("CurrentEL");
    ((current_el >> 2) & 0b11) as u8
}

/// Human-readable name for an exception level.
pub fn el_name(el: u8) -> &'static str {
    match el {
        0 => "User Space",
        1 => "Kernel Space",
        2 => "Hyper Space",
        3 => "Secure Monitor/Firmware",
        _ => "Invalid Exception Level",
    }
}

// --- CPU identification + PMU cycle counter ---------------------------------
//
// Snapshotted at boot (MIDR/CTR/feature regs don't change at runtime).

use crate::klib::sync::SpinLock;

struct CpuInfo {
    midr: u64,
    ctr: u64,
    pfr0: u64,
    isar0: u64,
    mmfr0: u64,
}

static CPU_INFO: SpinLock<CpuInfo> = SpinLock::new(CpuInfo {
    midr: 0,
    ctr: 0,
    pfr0: 0,
    isar0: 0,
    mmfr0: 0,
});

fn implementer_name(imp: u8) -> &'static str {
    match imp {
        0x41 => "ARM Limited",
        0x42 => "Broadcom",
        0x43 => "Cavium",
        0x46 => "Fujitsu",
        0x48 => "HiSilicon",
        0x49 => "Infineon",
        0x4D => "Motorola/Freescale",
        0x4E => "NVIDIA",
        0x50 => "Applied Micro",
        0x51 => "Qualcomm",
        0x53 => "Samsung",
        0x54 => "Texas Instruments",
        0x56 => "Marvell",
        0x61 => "Apple",
        0x66 => "Faraday",
        0x69 => "Intel",
        0xC0 => "Ampere",
        _ => "Unknown",
    }
}

fn arm_part_name(part: u16) -> &'static str {
    match part {
        0xD03 => "Cortex-A53",
        0xD05 => "Cortex-A55",
        0xD07 => "Cortex-A57",
        0xD08 => "Cortex-A72",
        0xD09 => "Cortex-A73",
        0xD0A => "Cortex-A75",
        0xD0B => "Cortex-A76",
        0xD0D => "Cortex-A77",
        0xD40 => "Neoverse-V1",
        0xD41 => "Cortex-A78",
        0xD49 => "Neoverse-N2",
        0xD4A => "Neoverse-E1",
        _ => "unknown",
    }
}

fn parange_bits(mmfr0: u64) -> u64 {
    match mmfr0 & 0xF {
        0 => 32,
        1 => 36,
        2 => 40,
        3 => 42,
        4 => 44,
        5 => 48,
        6 => 52,
        _ => 0,
    }
}

/// Snapshot ID registers and enable the PMU cycle counter. Call once at boot.
pub fn cpu_init() {
    let midr = mrs!("midr_el1");
    {
        let mut c = CPU_INFO.lock();
        c.midr = midr;
        c.ctr = mrs!("ctr_el0");
        c.pfr0 = mrs!("id_aa64pfr0_el1");
        c.isar0 = mrs!("id_aa64isar0_el1");
        c.mmfr0 = mrs!("id_aa64mmfr0_el1");
    }

    unsafe {
        // PMCR_EL0: LC | C (reset cycle) | P (reset events) | E (enable).
        msr!("pmcr_el0", (1 << 6) | (1 << 2) | (1 << 1) | (1 << 0));
        // Enable the dedicated cycle counter.
        msr!("pmcntenset_el0", 1 << 31);
    }

    let imp = ((midr >> 24) & 0xFF) as u8;
    let part = if imp == 0x41 {
        arm_part_name(((midr >> 4) & 0xFFF) as u16)
    } else {
        "unknown-part"
    };
    kprintln!(
        "[CPU] {} {} r{}p{}, midr={:#x}",
        implementer_name(imp),
        part,
        (midr >> 20) & 0xF,
        midr & 0xF,
        midr
    );
    kprintln!("[CPU] PMU enabled (PMCCNTR_EL0 live)");
}

/// Read the 64-bit cycle counter (PMCCNTR_EL0).
pub fn read_cycles() -> u64 {
    mrs!("pmccntr_el0")
}

/// Render a multi-line CPU description into `out`; returns bytes written.
pub fn render_info(out: &mut [u8]) -> usize {
    use core::fmt::Write;
    let c = CPU_INFO.lock();
    let midr = c.midr;
    let implementer = ((midr >> 24) & 0xFF) as u8;
    let variant = ((midr >> 20) & 0xF) as u8;
    let arch = ((midr >> 16) & 0xF) as u8;
    let partnum = ((midr >> 4) & 0xFFF) as u16;
    let revision = (midr & 0xF) as u8;

    let i_words = 1u64 << (c.ctr & 0xF);
    let d_words = 1u64 << ((c.ctr >> 16) & 0xF);

    let fp_field = (c.pfr0 >> 16) & 0xF;
    let simd_field = (c.pfr0 >> 20) & 0xF;
    let has_aes = (c.isar0 >> 4) & 0xF != 0;
    let has_sha1 = (c.isar0 >> 8) & 0xF != 0;
    let has_sha2 = (c.isar0 >> 12) & 0xF != 0;
    let has_crc32 = (c.isar0 >> 16) & 0xF != 0;
    let has_rndr = (c.isar0 >> 60) & 0xF != 0;
    let mmfr0 = c.mmfr0;
    let cycles = read_cycles();
    drop(c);

    let yn = |b: bool| if b { "yes" } else { "no" };
    let mut w = crate::klib::fmtbuf::FmtBuf::new(out);
    let _ = write!(
        w,
        "implementer  : {} ({:#x})\n\
         part         : {} ({:#x})\n\
         architecture : ARMv8 ({:#x})\n\
         variant      : {:#x}\n\
         revision     : {:#x}\n\
         midr_el1     : {:#x}\n\
         icache_line  : {} bytes\n\
         dcache_line  : {} bytes\n\
         phys_addr    : {} bits\n\
         fp           : {}\n\
         advsimd      : {}\n\
         aes          : {}\n\
         sha1         : {}\n\
         sha2         : {}\n\
         crc32        : {}\n\
         rndr         : {}\n\
         cycles       : {}\n",
        implementer_name(implementer),
        implementer,
        if implementer == 0x41 {
            arm_part_name(partnum)
        } else {
            "unknown"
        },
        partnum,
        arch,
        variant,
        revision,
        midr,
        i_words * 4,
        d_words * 4,
        parange_bits(mmfr0),
        yn(fp_field != 0xF),
        yn(simd_field != 0xF),
        yn(has_aes),
        yn(has_sha1),
        yn(has_sha2),
        yn(has_crc32),
        yn(has_rndr),
        cycles
    );
    w.len()
}

/// Print the current exception level over the UART.
///
/// Uses the UART's direct string helpers rather than `kprintln!`. Before the
/// MMU is enabled, RAM is treated as Device memory (strongly-ordered), so the
/// unaligned 2-byte accesses that `core::fmt`'s integer formatting emits fault
/// — and no exception vectors are installed yet. The aligned `puts`/`putc`
/// path is safe; `kprintln!`/`core::fmt` become usable once the MMU maps RAM as
/// Normal cacheable memory.
pub fn print_current_el() {
    let el = current_el();
    let uart = Uart;
    uart.puts("Current Exception Level: ");
    uart.println(el_name(el));
}
