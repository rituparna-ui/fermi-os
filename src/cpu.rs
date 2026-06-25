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

/// Reset the machine via PSCI SYSTEM_RESET (QEMU virt conduit = HVC).
pub fn system_reset() -> ! {
    unsafe {
        core::arch::asm!("hvc #0", in("x0") 0x8400_0009u64, options(nomem, nostack));
    }
    // Should not return; park if it does.
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}

pub fn print_current_el() {
    crate::uart::puts("Current Exception Level: ");
    crate::uart::println(el_name(current_el()));
}

// ---------------------------------------------------------------------------
// CPU identification + PMU cycle counter (port of the original cpu.c).
// All sysregs read here are standard ARMv8-A.
// ---------------------------------------------------------------------------

use crate::sync::Racy;

struct CpuId {
    midr: u64,
    ctr: u64,
    pfr0: u64,
    isar0: u64,
    mmfr0: u64,
}
static CPUID: Racy<CpuId> = Racy::new(CpuId {
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
        0x4E => "NVIDIA",
        0x50 => "Applied Micro",
        0x51 => "Qualcomm",
        0x53 => "Samsung",
        0x56 => "Marvell",
        0x61 => "Apple",
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
        0xD40 => "Neoverse-V1",
        0xD41 => "Cortex-A78",
        0xD49 => "Neoverse-N2",
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

/// Snapshot ID registers and enable the PMU cycle counter (PMCCNTR_EL0).
pub fn init() {
    let c = unsafe { CPUID.get() };
    c.midr = mrs!(midr_el1);
    c.ctr = mrs!(ctr_el0);
    c.pfr0 = mrs!(id_aa64pfr0_el1);
    c.isar0 = mrs!(id_aa64isar0_el1);
    c.mmfr0 = mrs!(id_aa64mmfr0_el1);
    // PMCR_EL0: LC(6) | C(2) reset | P(1) | E(0) enable.
    msr!(pmcr_el0, (1 << 6) | (1 << 2) | (1 << 1) | (1 << 0));
    msr!(pmcntenset_el0, 1u64 << 31);
}

/// Monotonic CPU cycle counter (PMCCNTR_EL0).
pub fn read_cycles() -> u64 {
    mrs!(pmccntr_el0)
}

/// Render a /proc/cpuinfo-style description.
pub fn render_info() -> alloc::string::String {
    use core::fmt::Write;
    let c = unsafe { CPUID.get() };
    let imp = ((c.midr >> 24) & 0xFF) as u8;
    let variant = ((c.midr >> 20) & 0xF) as u8;
    let part = ((c.midr >> 4) & 0xFFF) as u16;
    let revision = (c.midr & 0xF) as u8;
    let i_words = 1u64 << (c.ctr & 0xF);
    let d_words = 1u64 << ((c.ctr >> 16) & 0xF);
    let fp = (c.pfr0 >> 16) & 0xF;
    let simd = (c.pfr0 >> 20) & 0xF;
    let has = |shift: u64| (c.isar0 >> shift) & 0xF != 0;
    let yesno = |b: bool| if b { "yes" } else { "no" };

    let mut s = alloc::string::String::new();
    let _ = writeln!(s, "implementer  : {} ({:#x})", implementer_name(imp), imp);
    let _ = writeln!(
        s,
        "part         : {} ({:#x})",
        if imp == 0x41 { arm_part_name(part) } else { "unknown" },
        part
    );
    let _ = writeln!(s, "variant      : {:#x}", variant);
    let _ = writeln!(s, "revision     : {:#x}", revision);
    let _ = writeln!(s, "midr_el1     : {:#x}", c.midr);
    let _ = writeln!(s, "icache_line  : {} bytes", i_words * 4);
    let _ = writeln!(s, "dcache_line  : {} bytes", d_words * 4);
    let _ = writeln!(s, "phys_addr    : {} bits", parange_bits(c.mmfr0));
    let _ = writeln!(s, "fp           : {}", yesno(fp != 0xF));
    let _ = writeln!(s, "advsimd      : {}", yesno(simd != 0xF));
    let _ = writeln!(s, "aes          : {}", yesno(has(4)));
    let _ = writeln!(s, "sha1         : {}", yesno(has(8)));
    let _ = writeln!(s, "sha2         : {}", yesno(has(12)));
    let _ = writeln!(s, "crc32        : {}", yesno(has(16)));
    let _ = writeln!(s, "cycles       : {}", read_cycles());
    s
}
