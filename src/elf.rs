//! Minimal ELF64 loader for static aarch64 ET_EXEC binaries.
//!
//! Port of `src/syscall/elf.c`. Each PT_LOAD becomes a PMM allocation mapped
//! at its p_vaddr with W^X permissions derived from p_flags. Fields are parsed
//! by byte offset to avoid packed-struct alignment hazards.

use crate::kprintln;
use crate::mm::mmu::{self, USER_STACK_TOP};
use crate::mm::pmm::{self, PAGE_SIZE};

const ELF_MAX_REGIONS: usize = 4;
const MAX_PHNUM: u16 = 32;

const ET_EXEC: u16 = 2;
const EM_AARCH64: u16 = 0xB7;
const PT_LOAD: u32 = 1;
const PF_X: u32 = 1 << 0;
const PF_W: u32 = 1 << 1;
const PF_R: u32 = 1 << 2;

const PHDR_SIZE: usize = 56;

#[derive(Clone, Copy)]
pub struct ElfRegion {
    pub phys: u64,
    pub pages: u64,
}

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
    fn free_regions(&mut self) {
        for i in 0..self.region_count {
            let r = self.regions[i];
            if r.phys != 0 && r.pages != 0 {
                pmm::free_pages(r.phys, r.pages);
            }
        }
        self.region_count = 0;
    }
}

#[inline]
fn rd_u16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
#[inline]
fn rd_u32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
#[inline]
fn rd_u64(b: &[u8], o: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(a)
}

/// Translate ELF p_flags to user PTE flags (W^X enforced).
fn parse_flags(pf: u32) -> Option<u64> {
    let mut f = mmu::pte_attridx(1) | mmu::PTE_PXN;
    if pf & PF_W != 0 {
        f |= mmu::PTE_AP_RW_EL0;
    } else {
        f |= mmu::PTE_AP_RO_EL0;
    }
    if pf & PF_X == 0 {
        f |= mmu::PTE_UXN;
    }
    if pf & PF_W != 0 && pf & PF_X != 0 {
        return None; // reject W+X
    }
    if pf & PF_R == 0 {
        return None;
    }
    Some(f)
}

/// I/D cache sync for freshly-written executable bytes (DC CVAU / IC IVAU).
fn cpu_sync_icache(va: u64, size: u64) {
    if size == 0 {
        return;
    }
    let ctr: u64 = crate::mrs!(ctr_el0);
    let i_line = 4u64 << (ctr & 0xF);
    let d_line = 4u64 << ((ctr >> 16) & 0xF);
    let end = va + size;
    let mut p = va & !(d_line - 1);
    while p < end {
        unsafe { core::arch::asm!("dc cvau, {}", in(reg) p) };
        p += d_line;
    }
    unsafe { core::arch::asm!("dsb ish") };
    let mut p = va & !(i_line - 1);
    while p < end {
        unsafe { core::arch::asm!("ic ivau, {}", in(reg) p) };
        p += i_line;
    }
    unsafe { core::arch::asm!("dsb ish", "isb") };
}

/// Parse and load an ELF into `user_l0`. Returns the image on success.
pub fn load(buf: &[u8], user_l0: u64) -> Option<ElfImage> {
    if buf.len() < 64 {
        kprintln!("[ELF] file shorter than ELF header");
        return None;
    }
    if buf[0] != 0x7F || buf[1] != b'E' || buf[2] != b'L' || buf[3] != b'F' {
        kprintln!("[ELF] bad magic");
        return None;
    }
    if buf[4] != 2 || buf[5] != 1 || buf[6] != 1 {
        kprintln!("[ELF] not ELFCLASS64/LSB/CURRENT");
        return None;
    }
    if rd_u16(buf, 16) != ET_EXEC {
        kprintln!("[ELF] not ET_EXEC");
        return None;
    }
    if rd_u16(buf, 18) != EM_AARCH64 {
        kprintln!("[ELF] not EM_AARCH64");
        return None;
    }
    let e_entry = rd_u64(buf, 24);
    let e_phoff = rd_u64(buf, 32) as usize;
    let e_phentsize = rd_u16(buf, 54) as usize;
    let e_phnum = rd_u16(buf, 56);
    if e_phentsize != PHDR_SIZE {
        kprintln!("[ELF] unexpected e_phentsize");
        return None;
    }
    if e_phnum == 0 || e_phnum > MAX_PHNUM {
        kprintln!("[ELF] e_phnum out of range");
        return None;
    }
    if e_phoff + e_phnum as usize * PHDR_SIZE > buf.len() {
        kprintln!("[ELF] phdr table outside file");
        return None;
    }

    let mut img = ElfImage::empty();
    img.entry = e_entry;

    for i in 0..e_phnum as usize {
        let po = e_phoff + i * PHDR_SIZE;
        let p_type = rd_u32(buf, po);
        if p_type != PT_LOAD {
            continue;
        }
        let p_flags = rd_u32(buf, po + 4);
        let p_offset = rd_u64(buf, po + 8) as usize;
        let p_vaddr = rd_u64(buf, po + 16);
        let p_filesz = rd_u64(buf, po + 32);
        let p_memsz = rd_u64(buf, po + 40);

        if p_filesz > p_memsz {
            img.free_regions();
            return None;
        }
        if p_offset + p_filesz as usize > buf.len() {
            img.free_regions();
            return None;
        }
        if p_vaddr >= USER_STACK_TOP || p_vaddr + p_memsz > USER_STACK_TOP {
            kprintln!("[ELF] PT_LOAD vaddr outside user half");
            img.free_regions();
            return None;
        }
        if img.region_count >= ELF_MAX_REGIONS {
            img.free_regions();
            return None;
        }
        let pte_flags = match parse_flags(p_flags) {
            Some(f) => f,
            None => {
                kprintln!("[ELF] rejected p_flags (W+X or no R)");
                img.free_regions();
                return None;
            }
        };

        let va_lo = p_vaddr & !(PAGE_SIZE - 1);
        let va_hi = (p_vaddr + p_memsz + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        let pages = (va_hi - va_lo) / PAGE_SIZE;
        let phys = pmm::allocate_pages(pages);
        if phys == 0 {
            img.free_regions();
            return None;
        }
        let kva = mmu::phys_to_virt(phys);
        unsafe {
            core::ptr::write_bytes(kva as *mut u8, 0, (pages * PAGE_SIZE) as usize);
            if p_filesz > 0 {
                let intra = (p_vaddr - va_lo) as usize;
                core::ptr::copy_nonoverlapping(
                    buf.as_ptr().add(p_offset),
                    (kva as *mut u8).add(intra),
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
            i, p_vaddr, p_filesz, p_memsz,
            if p_flags & PF_R != 0 { "R" } else { "-" },
            if p_flags & PF_W != 0 { "W" } else { "-" },
            if p_flags & PF_X != 0 { "X" } else { "-" },
        );
    }

    if img.region_count == 0 {
        kprintln!("[ELF] no PT_LOAD segments");
        return None;
    }
    Some(img)
}
