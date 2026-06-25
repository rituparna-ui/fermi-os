//! Minimal ELF64 loader for static aarch64 ET_EXEC binaries.
//!
//! Each PT_LOAD becomes a separate PMM allocation mapped at its p_vaddr with
//! permissions from p_flags (RO+X text, RW+UXN data/bss, RO+UXN rodata; W+X
//! and R-less segments rejected). Executable segments get an icache sync
//! (DC CVAU / IC IVAU) since ARMv8 isn't I/D-coherent at PoU — load-bearing on
//! real hardware even though QEMU is self-coherent (risk R7).

use crate::klib::uart::Uart;
use crate::kprintln;
use crate::mm::consts::*;
use crate::mm::mmu;
use crate::mm::pmm;

const ELF_MAGIC: [u8; 4] = [0x7F, b'E', b'L', b'F'];
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const EV_CURRENT: u8 = 1;
const ET_EXEC: u16 = 2;
const EM_AARCH64: u16 = 0xB7;
const PT_LOAD: u32 = 1;
const PF_X: u32 = 1 << 0;
const PF_W: u32 = 1 << 1;
const PF_R: u32 = 1 << 2;

const MAX_PHNUM: u16 = 32;
pub const ELF_MAX_REGIONS: usize = 4;

/// One PT_LOAD's PMM allocation, tracked for freeing on reap/re-exec.
#[derive(Clone, Copy)]
pub struct ElfRegion {
    pub phys: u64,
    pub pages: u64,
}

/// Result of a load: entry VA + the per-segment allocations.
#[derive(Clone, Copy)]
pub struct ElfImage {
    pub entry: u64,
    pub region_count: usize,
    pub regions: [ElfRegion; ELF_MAX_REGIONS],
}

impl ElfImage {
    pub const fn empty() -> Self {
        Self {
            entry: 0,
            region_count: 0,
            regions: [ElfRegion { phys: 0, pages: 0 }; ELF_MAX_REGIONS],
        }
    }
}

// Field readers for the packed little-endian ELF structs (avoid unaligned refs).
fn rd_u16(buf: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([buf[off], buf[off + 1]])
}
fn rd_u32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}
fn rd_u64(buf: &[u8], off: usize) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&buf[off..off + 8]);
    u64::from_le_bytes(b)
}

const EHDR_SIZE: usize = 64;
const PHDR_SIZE: usize = 56;
// Ehdr field offsets.
const E_TYPE: usize = 16;
const E_MACHINE: usize = 18;
const E_ENTRY: usize = 24;
const E_PHOFF: usize = 32;
const E_PHENTSIZE: usize = 54;
const E_PHNUM: usize = 56;
// Phdr field offsets.
const P_TYPE: usize = 0;
const P_FLAGS: usize = 4;
const P_OFFSET: usize = 8;
const P_VADDR: usize = 16;
const P_FILESZ: usize = 32;
const P_MEMSZ: usize = 40;

/// Synchronize icache with dcache over a freshly-written executable range
/// (DC CVAU per D-line, DSB, IC IVAU per I-line, DSB, ISB). Line sizes from
/// CTR_EL0.
fn cpu_sync_icache(va: u64, size: u64) {
    if size == 0 {
        return;
    }
    let ctr = crate::mrs!("ctr_el0");
    let i_line = 4u64 << (ctr & 0xF);
    let d_line = 4u64 << ((ctr >> 16) & 0xF);
    let end = va + size;

    let mut p = va & !(d_line - 1);
    while p < end {
        unsafe { core::arch::asm!("dc cvau, {}", in(reg) p, options(nostack, preserves_flags)) };
        p += d_line;
    }
    unsafe { core::arch::asm!("dsb ish") };

    let mut p = va & !(i_line - 1);
    while p < end {
        unsafe { core::arch::asm!("ic ivau, {}", in(reg) p, options(nostack, preserves_flags)) };
        p += i_line;
    }
    unsafe { core::arch::asm!("dsb ish", "isb") };
}

/// Derive PTE flags from ELF p_flags. PXN always set; UXN set unless executable.
/// Rejects W+X and R-less segments. Returns None on rejection.
fn parse_flags(pf: u32) -> Option<u64> {
    let mut f = pte_attridx(1) | PTE_PXN;
    if pf & PF_W != 0 {
        f |= PTE_AP_RW_EL0;
    } else {
        f |= PTE_AP_RO_EL0;
    }
    if pf & PF_X == 0 {
        f |= PTE_UXN;
    }
    if (pf & PF_W != 0) && (pf & PF_X != 0) {
        return None; // strict W^X
    }
    if pf & PF_R == 0 {
        return None;
    }
    Some(f)
}

fn free_regions(img: &mut ElfImage) {
    for i in 0..img.region_count {
        let r = img.regions[i];
        if r.phys != 0 && r.pages != 0 {
            pmm::free_pages(r.phys, r.pages);
        }
    }
    img.region_count = 0;
}

/// Parse `buf` as a static aarch64 ET_EXEC ELF, allocate + map each PT_LOAD
/// into `user_l0`, and return the image on success (Err on any failure, with
/// partial allocations freed).
pub fn load(buf: &[u8], user_l0: u64) -> Result<ElfImage, ()> {
    let uart = Uart;
    if buf.len() < EHDR_SIZE {
        uart.errorln("[ELF] file shorter than ELF header");
        return Err(());
    }

    if buf[0..4] != ELF_MAGIC {
        uart.errorln("[ELF] bad magic");
        return Err(());
    }
    if buf[4] != ELFCLASS64 {
        uart.errorln("[ELF] not ELFCLASS64");
        return Err(());
    }
    if buf[5] != ELFDATA2LSB {
        uart.errorln("[ELF] not ELFDATA2LSB");
        return Err(());
    }
    if buf[6] != EV_CURRENT {
        uart.errorln("[ELF] bad EI_VERSION");
        return Err(());
    }
    if rd_u16(buf, E_TYPE) != ET_EXEC {
        uart.errorln("[ELF] not ET_EXEC (PIE / shared not supported)");
        return Err(());
    }
    if rd_u16(buf, E_MACHINE) != EM_AARCH64 {
        uart.errorln("[ELF] not EM_AARCH64");
        return Err(());
    }
    if rd_u16(buf, E_PHENTSIZE) as usize != PHDR_SIZE {
        uart.errorln("[ELF] unexpected e_phentsize");
        return Err(());
    }

    let phnum = rd_u16(buf, E_PHNUM);
    if phnum == 0 || phnum > MAX_PHNUM {
        uart.errorln("[ELF] e_phnum out of range");
        return Err(());
    }
    let phoff = rd_u64(buf, E_PHOFF);
    let phtab_end = phoff + phnum as u64 * PHDR_SIZE as u64;
    if phoff > buf.len() as u64 || phtab_end > buf.len() as u64 {
        uart.errorln("[ELF] phdr table outside file");
        return Err(());
    }

    let entry = rd_u64(buf, E_ENTRY);
    let mut img = ElfImage::empty();
    img.entry = entry;

    for i in 0..phnum {
        let ph = phoff as usize + i as usize * PHDR_SIZE;
        if rd_u32(buf, ph + P_TYPE) != PT_LOAD {
            continue;
        }
        let p_flags = rd_u32(buf, ph + P_FLAGS);
        let p_offset = rd_u64(buf, ph + P_OFFSET);
        let p_vaddr = rd_u64(buf, ph + P_VADDR);
        let p_filesz = rd_u64(buf, ph + P_FILESZ);
        let p_memsz = rd_u64(buf, ph + P_MEMSZ);

        if p_filesz > p_memsz {
            uart.errorln("[ELF] PT_LOAD: filesz > memsz");
            free_regions(&mut img);
            return Err(());
        }
        if p_offset + p_filesz > buf.len() as u64 || p_offset + p_filesz < p_offset {
            uart.errorln("[ELF] PT_LOAD: file slice outside buffer");
            free_regions(&mut img);
            return Err(());
        }
        if p_vaddr >= USER_STACK_TOP
            || p_vaddr + p_memsz > USER_STACK_TOP
            || p_vaddr + p_memsz < p_vaddr
        {
            uart.errorln("[ELF] PT_LOAD: vaddr range outside user half");
            free_regions(&mut img);
            return Err(());
        }
        if img.region_count >= ELF_MAX_REGIONS {
            uart.errorln("[ELF] too many PT_LOAD segments");
            free_regions(&mut img);
            return Err(());
        }
        let pte_flags = match parse_flags(p_flags) {
            Some(f) => f,
            None => {
                uart.errorln("[ELF] PT_LOAD: rejected p_flags (W+X or no R)");
                free_regions(&mut img);
                return Err(());
            }
        };

        // Page-aligned span covering [vaddr, vaddr+memsz).
        let va_lo = p_vaddr & !(PAGE_SIZE - 1);
        let va_hi = (p_vaddr + p_memsz + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        let pages = (va_hi - va_lo) / PAGE_SIZE;
        let phys = pmm::allocate_pages(pages);
        if phys == 0 {
            uart.errorln("[ELF] PT_LOAD: PMM allocation failed");
            free_regions(&mut img);
            return Err(());
        }

        // Zero, then copy file bytes at the intra-page offset (.bss stays zero).
        let kva = phys_to_virt(phys);
        unsafe {
            core::ptr::write_bytes(kva as *mut u8, 0, (pages * PAGE_SIZE) as usize);
            if p_filesz > 0 {
                let intra = p_vaddr - va_lo;
                core::ptr::copy_nonoverlapping(
                    buf.as_ptr().add(p_offset as usize),
                    (kva + intra) as *mut u8,
                    p_filesz as usize,
                );
            }
        }

        if p_flags & PF_X != 0 {
            cpu_sync_icache(kva, pages * PAGE_SIZE);
        }

        mmu::map_user_range(user_l0, va_lo, phys, pages, pte_flags);

        img.regions[img.region_count] = ElfRegion { phys, pages };
        img.region_count += 1;

        kprintln!(
            "[ELF] PT_LOAD #{}: vaddr={:#x} filesz={} memsz={} flags={}{}{}",
            i,
            p_vaddr,
            p_filesz,
            p_memsz,
            if p_flags & PF_R != 0 { "R" } else { "-" },
            if p_flags & PF_W != 0 { "W" } else { "-" },
            if p_flags & PF_X != 0 { "X" } else { "-" }
        );
    }

    if img.region_count == 0 {
        uart.errorln("[ELF] no PT_LOAD segments — nothing to run");
        return Err(());
    }

    // Entry must lie inside an executable PT_LOAD.
    let mut entry_ok = false;
    for i in 0..phnum {
        let ph = phoff as usize + i as usize * PHDR_SIZE;
        if rd_u32(buf, ph + P_TYPE) != PT_LOAD {
            continue;
        }
        let p_vaddr = rd_u64(buf, ph + P_VADDR);
        let p_memsz = rd_u64(buf, ph + P_MEMSZ);
        let p_flags = rd_u32(buf, ph + P_FLAGS);
        if entry >= p_vaddr && entry < p_vaddr + p_memsz && p_flags & PF_X != 0 {
            entry_ok = true;
            break;
        }
    }
    if !entry_ok {
        uart.errorln("[ELF] e_entry not inside an executable PT_LOAD");
        free_regions(&mut img);
        return Err(());
    }

    Ok(img)
}
