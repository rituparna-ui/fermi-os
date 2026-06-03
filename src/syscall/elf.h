#ifndef SYSCALL_ELF_H
#define SYSCALL_ELF_H

#include <stddef.h>
#include <stdint.h>

/* Minimal ELF64 definitions sufficient for loading static aarch64 ET_EXEC
 * binaries. We deliberately don't pull in the system <elf.h> \u2014 we want
 * a self-contained vocabulary that's obvious at-a-glance for a kernel
 * reviewer. */

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

#define ET_EXEC       2
#define EM_AARCH64    0xB7

#define PT_NULL       0
#define PT_LOAD       1
#define PT_PHDR       6

/* p_flags */
#define PF_X          (1u << 0)
#define PF_W          (1u << 1)
#define PF_R          (1u << 2)

typedef struct __attribute__((packed)) {
  unsigned char e_ident[16];
  uint16_t      e_type;
  uint16_t      e_machine;
  uint32_t      e_version;
  uint64_t      e_entry;
  uint64_t      e_phoff;
  uint64_t      e_shoff;
  uint32_t      e_flags;
  uint16_t      e_ehsize;
  uint16_t      e_phentsize;
  uint16_t      e_phnum;
  uint16_t      e_shentsize;
  uint16_t      e_shnum;
  uint16_t      e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

/* Output of the loader: one entry per PT_LOAD that the loader allocated and
 * mapped. The kernel keeps these so it can free per-exec PMM allocations
 * when the task is reaped or exec()s again. */
#define ELF_MAX_REGIONS 4

typedef struct {
  uintptr_t phys;   /* PMM-allocated physical base for this region */
  uint64_t  pages;  /* number of pages allocated (== ceil(memsz / 4 KiB)) */
} elf_region_t;

typedef struct {
  uint64_t      entry;          /* virtual address of the program entry */
  int           region_count;   /* number of valid entries in regions[] */
  elf_region_t  regions[ELF_MAX_REGIONS];
} elf_image_t;

/* Parse `kbuf` (size `size`) as a static aarch64 ET_EXEC ELF, allocate
 * PMM pages for each PT_LOAD, copy/zero contents, and map them into the
 * given user_l0 with permissions derived from the segment's p_flags.
 *
 * On success returns 0 and fills *out. On failure returns -1; any PMM
 * allocations made before the failure are freed before return so the
 * caller doesn't need to clean up partial state.
 *
 * Constraints we enforce:
 *   - first 4 bytes of e_ident match \x7fELF
 *   - ELFCLASS64, ELFDATA2LSB, EV_CURRENT
 *   - e_type == ET_EXEC, e_machine == EM_AARCH64
 *   - e_phnum <= some sane bound (32)
 *   - At most ELF_MAX_REGIONS PT_LOAD segments
 *   - Each PT_LOAD's [p_offset, p_offset+p_filesz) must lie inside the
 *     supplied buffer
 *   - p_memsz >= p_filesz
 *   - p_vaddr range must be in the user half (< USER_STACK_TOP)
 */
int elf_load(const uint8_t *kbuf, size_t size, uint64_t *user_l0,
             elf_image_t *out);

#endif
