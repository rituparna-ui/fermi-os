#include "elf.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include "utils/utils.h"

/* ---------------------------------------------------------------------------
 * elf_load — minimal ELF64 loader for aarch64 static ET_EXEC binaries.
 *
 * Each PT_LOAD becomes a separate PMM allocation mapped at its p_vaddr
 * with permissions derived from p_flags:
 *   PF_R | PF_X       -> RO + EL0-X    (.text, .rodata folded into text)
 *   PF_R | PF_W       -> RW + UXN      (.data + .bss)
 *   PF_R              -> RO + UXN      (read-only data, rare)
 * Anything else is rejected (we don't support mixed W+X today).
 *
 * The loader rounds page allocations up to a 4 KiB granule \u2014 same granule
 * mmu_map_user_range() emits L3 PTEs for. Sub-page misalignment between
 * p_vaddr and the page boundary is handled by allocating to cover the
 * whole p_vaddr..p_vaddr+p_memsz range and copying file bytes at the
 * correct intra-page offset.
 * --------------------------------------------------------------------------- */

/* Synchronize the icache with the dcache for a freshly-written executable
 * range. ARMv8 does NOT guarantee I-D coherence at PoU; spec requires:
 *   1. DC CVAU per D-line  — clean dcache to PoU
 *   2. DSB ISH               — wait for clean to complete
 *   3. IC IVAU per I-line   — invalidate icache lines to PoU
 *   4. DSB ISH + ISB         — wait for invalidate, flush prefetch
 *
 * Without this, after we memcpy ELF code bytes via the kernel TTBR1 alias
 * the CPU may still hold stale (zero / prior allocation) icache lines
 * for the same physical address when EL0 fetches via the TTBR0 mapping.
 * Works on QEMU because its icache is self-coherent; would silently fail
 * on a real Cortex-A72.
 *
 * Cache line sizes are read from CTR_EL0 — the architectural way to
 * discover them rather than hard-coding 64. */
static void cpu_sync_icache(uintptr_t va, size_t size) {
  if (size == 0) return;

  uint64_t ctr;
  __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
  /* CTR_EL0.IminLine[3:0]   = log2(words/I-line); word = 4 bytes */
  /* CTR_EL0.DminLine[19:16] = log2(words/D-line) */
  size_t i_line = (size_t)4 << ((ctr >>  0) & 0xF);
  size_t d_line = (size_t)4 << ((ctr >> 16) & 0xF);

  uintptr_t end = va + size;

  for (uintptr_t p = va & ~((uintptr_t)d_line - 1); p < end; p += d_line) {
    __asm__ __volatile__("dc cvau, %0" :: "r"(p) : "memory");
  }
  __asm__ __volatile__("dsb ish" ::: "memory");

  for (uintptr_t p = va & ~((uintptr_t)i_line - 1); p < end; p += i_line) {
    __asm__ __volatile__("ic ivau, %0" :: "r"(p) : "memory");
  }
  __asm__ __volatile__("dsb ish" ::: "memory");
  __asm__ __volatile__("isb" ::: "memory");
}


#define MAX_PHNUM 32

static int parse_flags(uint32_t pf, uint64_t *out) {
  /* PXN is always set (kernel must never execute user pages). UXN is set
   * when PF_X is clear so user-mode can't execute non-text data. */
  uint64_t f = PTE_ATTRIDX(1) | PTE_PXN;

  if (pf & PF_W) {
    f |= PTE_AP_RW_EL0;
  } else {
    f |= PTE_AP_RO_EL0;
  }
  if (!(pf & PF_X)) {
    f |= PTE_UXN;
  }

  /* Reject W+X — we want strict W^X for user images. C-style binaries
   * never need this; if someone really wants to JIT later, this is the
   * exact line to revisit. */
  if ((pf & PF_W) && (pf & PF_X)) {
    return -1;
  }

  /* Reject pages with no R bit set: nothing useful would be in there
   * (and ARMv8 can't really express "X without R" for userspace anyway). */
  if (!(pf & PF_R)) {
    return -1;
  }

  *out = f;
  return 0;
}

static void free_regions(elf_image_t *img) {
  for (int i = 0; i < img->region_count; i++) {
    if (img->regions[i].phys && img->regions[i].pages) {
      pmm_free_pages(img->regions[i].phys, img->regions[i].pages);
    }
  }
  img->region_count = 0;
}

int elf_load(const uint8_t *kbuf, size_t size, uint64_t *user_l0,
             elf_image_t *out) {
  if (size < sizeof(Elf64_Ehdr)) {
    uart_errorln("[ELF] file shorter than ELF header");
    return -1;
  }
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kbuf;

  /* 1. Magic + class + data + version. */
  if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
      eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
    uart_errorln("[ELF] bad magic");
    return -1;
  }
  if (eh->e_ident[4] != ELFCLASS64) {
    uart_errorln("[ELF] not ELFCLASS64");
    return -1;
  }
  if (eh->e_ident[5] != ELFDATA2LSB) {
    uart_errorln("[ELF] not ELFDATA2LSB");
    return -1;
  }
  if (eh->e_ident[6] != EV_CURRENT) {
    uart_errorln("[ELF] bad EI_VERSION");
    return -1;
  }

  /* 2. Type + machine. */
  if (eh->e_type != ET_EXEC) {
    uart_errorln("[ELF] not ET_EXEC (PIE / shared not supported)");
    return -1;
  }
  if (eh->e_machine != EM_AARCH64) {
    uart_errorln("[ELF] not EM_AARCH64");
    return -1;
  }

  /* 3. Program-header table sanity. */
  if (eh->e_phentsize != sizeof(Elf64_Phdr)) {
    uart_errorln("[ELF] unexpected e_phentsize");
    return -1;
  }
  if (eh->e_phnum == 0 || eh->e_phnum > MAX_PHNUM) {
    uart_errorln("[ELF] e_phnum out of range");
    return -1;
  }
  if (eh->e_phoff > size ||
      eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size) {
    uart_errorln("[ELF] phdr table outside file");
    return -1;
  }

  out->entry        = eh->e_entry;
  out->region_count = 0;

  const Elf64_Phdr *phs = (const Elf64_Phdr *)(kbuf + eh->e_phoff);

  /* 4. Walk PT_LOAD segments. Allocate, copy, zero, map. */
  for (int i = 0; i < eh->e_phnum; i++) {
    const Elf64_Phdr *ph = &phs[i];
    if (ph->p_type != PT_LOAD) {
      continue;
    }

    /* Sanity-check the segment's claims about the file. */
    if (ph->p_filesz > ph->p_memsz) {
      uart_errorln("[ELF] PT_LOAD: filesz > memsz");
      free_regions(out);
      return -1;
    }
    if (ph->p_offset + ph->p_filesz > size ||
        ph->p_offset + ph->p_filesz < ph->p_offset /* overflow */) {
      uart_errorln("[ELF] PT_LOAD: file slice outside buffer");
      free_regions(out);
      return -1;
    }
    if (ph->p_vaddr >= USER_STACK_TOP ||
        ph->p_vaddr + ph->p_memsz > USER_STACK_TOP ||
        ph->p_vaddr + ph->p_memsz < ph->p_vaddr /* overflow */) {
      uart_errorln("[ELF] PT_LOAD: vaddr range outside user half");
      free_regions(out);
      return -1;
    }
    if (out->region_count >= ELF_MAX_REGIONS) {
      uart_errorln("[ELF] too many PT_LOAD segments");
      free_regions(out);
      return -1;
    }

    /* Permission bits. Reject anything we don't like before allocating
     * pages. */
    uint64_t pte_flags;
    if (parse_flags(ph->p_flags, &pte_flags) < 0) {
      uart_errorln("[ELF] PT_LOAD: rejected p_flags (W+X or no R)");
      free_regions(out);
      return -1;
    }

    /* Page-aligned span covering [vaddr, vaddr+memsz). */
    uint64_t va_lo   = ph->p_vaddr & ~(PAGE_SIZE - 1);
    uint64_t va_hi   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) &
                       ~(PAGE_SIZE - 1);
    uint64_t pages   = (va_hi - va_lo) / PAGE_SIZE;
    uintptr_t phys   = pmm_allocate_pages(pages);
    if (!phys) {
      uart_errorln("[ELF] PT_LOAD: PMM allocation failed");
      free_regions(out);
      return -1;
    }

    /* Zero the entire allocation first, then memcpy the file bytes at
     * the correct intra-page offset. The leftover bytes (.bss) stay zero. */
    uintptr_t kva = PHYS_TO_VIRT(phys);
    memset((void *)kva, 0, pages * PAGE_SIZE);
    if (ph->p_filesz > 0) {
      uint64_t intra = ph->p_vaddr - va_lo; /* offset of vaddr inside first page */
      memcpy((void *)(kva + intra), kbuf + ph->p_offset,
             (size_t)ph->p_filesz);
    }

    /* Sync icache for executable segments. We just wrote new instructions
     * via the TTBR1 kernel alias — EL0 will fetch them via TTBR0 next, so
     * dirty dcache lines must reach PoU and stale icache lines must be
     * flushed. Non-executable segments don't need this. */
    if (ph->p_flags & PF_X) {
      cpu_sync_icache(kva, pages * PAGE_SIZE);
    }


    /* Map into the user_l0 at the page-aligned va_lo. */
    mmu_map_user_range(user_l0, va_lo, phys, pages, pte_flags);

    out->regions[out->region_count].phys  = phys;
    out->regions[out->region_count].pages = pages;
    out->region_count++;

    uart_printf("[ELF] PT_LOAD #%d: vaddr=%x filesz=%d memsz=%d flags=%c%c%c\n",
                (uint64_t)i, ph->p_vaddr, ph->p_filesz, ph->p_memsz,
                (ph->p_flags & PF_R) ? 'R' : '-',
                (ph->p_flags & PF_W) ? 'W' : '-',
                (ph->p_flags & PF_X) ? 'X' : '-');
  }

  if (out->region_count == 0) {
    uart_errorln("[ELF] no PT_LOAD segments \u2014 nothing to run");
    return -1;
  }

  /* Entry point must lie inside one of the regions we just mapped. */
  int entry_ok = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    const Elf64_Phdr *ph = &phs[i];
    if (ph->p_type != PT_LOAD) continue;
    if (eh->e_entry >= ph->p_vaddr &&
        eh->e_entry <  ph->p_vaddr + ph->p_memsz &&
        (ph->p_flags & PF_X)) {
      entry_ok = 1;
      break;
    }
  }
  if (!entry_ok) {
    uart_errorln("[ELF] e_entry not inside an executable PT_LOAD");
    free_regions(out);
    return -1;
  }

  return 0;
}
