#ifndef HYP_FDT_H
#define HYP_FDT_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Minimal flattened-device-tree (DTB) builder for foreign guests.
 *
 * A non-FermiOS guest follows the standard AArch64 boot protocol: it is entered
 * with x0 = physical address of a DTB and discovers its hardware by parsing it.
 * This builder emits a small but spec-valid DTB (big-endian, magic 0xd00dfeed)
 * describing the guest's RAM and a PL011 UART, so the hypervisor isn't tied to
 * any one guest's hardcoded assumptions.
 * ------------------------------------------------------------------------- */

#define FDT_MAGIC 0xd00dfeedU

/* Build a DTB into `buf` (<= cap bytes) describing a guest with `mem_size`
 * bytes of RAM at `mem_base` and a PL011 UART at `uart_base`. Returns the total
 * DTB size in bytes, or 0 on overflow. */
uint32_t fdt_build(void *buf, uint32_t cap, uint64_t mem_base,
                   uint64_t mem_size, uint64_t uart_base);

/* Validate a DTB header in `buf` (magic + sane totalsize). Returns the
 * big-endian-decoded totalsize, or 0 if invalid. Used as a hypervisor-side
 * self-check that the blob we built parses back. */
uint32_t fdt_check(const void *buf);

#endif /* HYP_FDT_H */
