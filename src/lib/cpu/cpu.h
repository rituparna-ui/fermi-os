#ifndef LIB_CPU_H
#define LIB_CPU_H

#include <stdint.h>
#include <stddef.h>

/* CPU identification + feature detection + PMU access. All paths use
 * architecturally-defined EL1 sysregs \u2014 no hardware-specific assumptions.
 *
 * cpu_init() is called once at boot from kernel_main; subsequent reads
 * (cpu_read_cycles, cpu_render_info) are safe from any context. */
void cpu_init(void);

/* Snapshot of the cycle counter (PMCCNTR_EL0). Monotonically increases
 * at the CPU clock rate while the core is awake. Wraps at 2\u2076\u2074. */
uint64_t cpu_read_cycles(void);

/* Format a multi-line, human-readable CPU description into `buf`. Returns
 * the number of bytes written (excluding terminating NUL), or a value
 * larger than `len` if truncated (same convention as ksnprintf). */
int cpu_render_info(char *buf, size_t len);

#endif
