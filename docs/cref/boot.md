# Boot Subsystem Specification

## Overview

The boot subsystem handles the aarch64 bare-metal initialization sequence from power-on reset through MMU enablement and handoff to the upper-half kernel (`kernel_main`). It comprises:

1. **boot.S**: AArch64 assembly entry point that configures physical addressing, stacks, enables exceptions and MMU, then transfers control to the higher-half kernel.
2. **kernel.c early_init()**: First C code executed in physical address space (PAS). Zeros BSS, enables FP/SIMD, initializes UART, exceptions, PMM, and MMU before returning to assembly.
3. **kernel.c kernel_main()**: Kernel entry point in virtual address space (VAS upper half, accessed via TTBR1). Initializes all subsystems, spawns tasks, and enters the idle loop.

**Key flow:**
```
power-on → _start (boot.S, physical) → early_init() → MMU enable → 
kernel_main() (upper-half) → subsystem init → task spawning → idle
```

## Hardware Constants

### Kernel Virtual Address Offset
```c
#define KERNEL_VA_OFFSET 0xFFFF000000000000ULL
```
All kernel code and data are linked at VA `0xFFFF_0000_0000_0000 + PA`. Subtract this offset to recover physical addresses during early boot before MMU is live.

### Memory Layout (PMM)
```c
#define MEM_START 0x40000000ULL        // Physical RAM base (1 GB)
#define MEM_SIZE  (8ULL * 1024 * 1024 * 1024)  // 8 GB total
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
```

### User-Space Layout
```c
#define USER_TEXT_BASE   0x00400000ULL    // 4 MB — user code start
#define USER_STACK_TOP   0x00800000ULL    // 8 MB — user stack top (TTBR0)
#define USER_STACK_PAGES 4                // 16 KiB initial user stack
```

### UART (PL011)
```c
#define UART_BASE  0x09000000UL
#define UART_DR    (UART_BASE + 0x00)  // Data Register
#define UART_FR    (UART_BASE + 0x18)  // Flag Register
#define UART_IBRD  (UART_BASE + 0x24)  // Integer Baud Rate Divisor
#define UART_FBRD  (UART_BASE + 0x28)  // Fractional Baud Rate Divisor
#define UART_LCRH  (UART_BASE + 0x2C)  // Line Control Register
#define UART_CR    (UART_BASE + 0x30)  // Control Register
#define UART_ICR   (UART_BASE + 0x44)  // Interrupt Clear Register
```

### ARM SYSREGS (read via MRS / write via MSR)

**Early boot (used directly in kernel.c):**
- `CPACR_EL1[20:21]` = `0b11` to enable FP/SIMD in EL1 (GCC varargs needs SIMD regs)
- `TTBR1_EL1` : kernel-space page table base (TTBR1 always maps upper half)
- `TTBR0_EL1` : user-space page table base (per-task, tagged with ASID)
- `TCR_EL1` : translation control (granule size, address space size, etc.)
- `MAIR_EL1` : memory attribute indirection

**User exception level detection (early_init):**
- `CurrentEL[3:2]` : current exception level (0=EL0, 1=EL1, 2=EL2, 3=EL3)

**Performance monitoring (cpu_init, called from kernel_main):**
- `PMCR_EL0[6]` : LC (long-counter 64-bit mode)
- `PMCR_EL0[2]` : C (reset cycle counter)
- `PMCR_EL0[1]` : P (reset event counters)
- `PMCR_EL0[0]` : E (enable counters)
- `PMCNTENSET_EL0[31]` : enable dedicated cycle counter (PMCCNTR_EL0)
- `PMCCNTR_EL0` : 64-bit cycle counter

**Exception handling (set by exceptions_init, relocate by exceptions_init_upper):**
- `VBAR_EL1` : vector base address register (trap table)
- `ESR_EL1` : exception syndrome register (decoded for fault diagnosis)
- `FAR_EL1` : fault address register

### PTE Bitfields (MMU, from mm/mmu/mmu.h)
```c
#define PTE_VALID        (1ULL << 0)      // Descriptor valid
#define PTE_TABLE        (1ULL << 1)      // Table entry (vs. block)
#define PTE_BLOCK        (0ULL << 1)      // Block entry
#define PTE_AF           (1ULL << 10)     // Access flag (set to avoid access fault)
#define PTE_SH_INNER     (3ULL << 8)      // Shareability: inner shareable
#define PTE_AP_RW        (0ULL << 6)      // EL1 RW, EL0 no access
#define PTE_AP_RW_EL0    (1ULL << 6)      // EL1 RW, EL0 RW
#define PTE_AP_RO        (2ULL << 6)      // EL1 RO, EL0 no access
#define PTE_AP_RO_EL0    (3ULL << 6)      // EL1 RO, EL0 RO
#define PTE_ATTRIDX(idx) ((idx) << 2)     // Memory type index from MAIR_EL1
#define PTE_UXN          (1ULL << 54)     // User (EL0) execute never
#define PTE_PXN          (1ULL << 53)     // Privileged (EL1) execute never
#define PTE_NG           (1ULL << 11)     // Non-global (ASID tagged)
#define PTE_ADDR_MASK    0x0000FFFFFFFFF000ULL  // 4 KB-aligned PA/next-table
```

### MMU Index Macros (for 4 KB granule, 48-bit output address)
```c
#define L0_INDEX(va) (((va) >> 39) & 0x1FF)
#define L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define L2_INDEX(va) (((va) >> 21) & 0x1FF)
#define L3_INDEX(va) (((va) >> 12) & 0x1FF)
```
Each page table has 512 entries (9-bit index).

### TTBR (Translation Table Base Register) Encoding
```c
#define TTBR_ASID_SHIFT 48
#define TTBR_BADDR_MASK 0x0000FFFFFFFFFFFFULL

// When TCR_EL1.AS=1 (16-bit ASID):
//   bits[63:48] = ASID (address space ID, per-task)
//   bits[47:1]  = page-table base address (page-aligned, so [11:0] = 0)
//   bit[0]      = CnP (cache near Phys, typically 0)

static inline uint64_t ttbr_pack(uint64_t baddr, uint16_t asid) {
  return (baddr & TTBR_BADDR_MASK) | ((uint64_t)asid << TTBR_ASID_SHIFT);
}
```

### Exception Frame (trap_frame_t, from exception.h)
Saved on stack during exception entry (by vector.S); layout must match assembly:
```c
typedef struct trap_frame {
  uint64_t regs[31];  // X0-X30 (X31 is SP, saved separately)
  uint64_t elr;       // Exception Link Register (return address)
  uint64_t spsr;      // Saved Processor State Register
  uint64_t esr;       // Exception Syndrome Register (fault cause code)
  uint64_t far;       // Fault Address Register (virtual address of fault)
} trap_frame_t;
```

### Exception Syndrome Decoding (ESR_EL1)
```c
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK  (0x3FULL << 26)
#define ESR_EC(esr)  (((esr) >> 26) & 0x3F)

// Exception classes
#define EC_UNKNOWN         0x00
#define EC_WF_TRAPPED      0x01   // WFI/WFE trapped
#define EC_SVC_AARCH64     0x15   // Syscall (SVC #imm)
#define EC_HVC_AARCH64     0x16   // Hypervisor call
#define EC_SMC_AARCH64     0x17   // Secure monitor call
#define EC_INST_ABORT_LO   0x20   // Instruction abort from lower EL
#define EC_INST_ABORT_CUR  0x21   // Instruction abort at current EL
#define EC_PC_ALIGN        0x22   // PC misaligned
#define EC_DATA_ABORT_LO   0x24   // Data abort from lower EL
#define EC_DATA_ABORT_CUR  0x25   // Data abort at current EL
#define EC_SP_ALIGN        0x26   // Stack pointer misaligned
#define EC_FP_AARCH64      0x2C   // Floating-point exception
#define EC_SERROR          0x2F   // System error abort
#define EC_BRK             0x3C   // Software breakpoint

// For data/instruction aborts, decode the ISS (Instruction Specific Syndrome) field
#define ESR_ISS_DFSC(esr)   ((esr) & 0x3F)        // Data Fault Status Code [5:0]
#define ESR_ISS_WNR(esr)    (((esr) >> 6) & 0x1)  // Write not Read [6]
#define ESR_ISS_CM(esr)     (((esr) >> 8) & 0x1)  // Cache maintenance [8]
#define ESR_ISS_S1PTW(esr)  (((esr) >> 7) & 0x1)  // Stage-1 fault on stage-2 walk [7]
#define ESR_ISS_EA(esr)     (((esr) >> 9) & 0x1)  // External abort [9]

// DFSC (Data Fault Status Code) values
#define DFSC_TRANSLATION_FAULT_L1  0x05
#define DFSC_TRANSLATION_FAULT_L2  0x06
#define DFSC_TRANSLATION_FAULT_L3  0x07
```

## Public API

### From boot.S (assembly entry)
```asm
.global _start
// Linked at KERNEL_VA_OFFSET (upper half).
// Entry point after power-on reset (PC is physical).
// Sets up physical stack, calls early_init, enables MMU, jumps to kernel_main.
```

### From kernel.c

#### early_init() — First C code executed
```c
void early_init(void);
// Preconditions:
//   - CPU is in EL1 (privileged mode)
//   - PC is physical (MMU off)
//   - stack pointer initialized by boot.S to physical address
//   - TTBR0_EL1 / TTBR1_EL1 may contain garbage
// Actions:
//   1. zero_bss() — zero the BSS segment ([__bss_start, __bss_end))
//   2. enable_fp_simd() — set CPACR_EL1.FPEN = 0b11 (FP/SIMD usable in EL1)
//   3. uart_init() — configure PL011 UART, print banner
//   4. print_current_el() — read CurrentEL, print exception level name
//   5. exceptions_init() — initialize exception table (vector.S), set VBAR_EL1
//   6. pmm_init(MEM_START, MEM_SIZE) — initialize physical memory allocator
//   7. pmm_print_info() — print free/used page counts
//   8. mmu_init() — set up kernel page tables (L0, L1, L2, L3), enable MMU
//   9. mmu_run_tests(l1_phys) — verify MMU with self-tests (safe before user tasks)
// Postconditions:
//   - BSS is zeroed
//   - FP/SIMD is enabled
//   - UART is initialized and printing
//   - Exception table is installed
//   - PMM is initialized
//   - MMU is LIVE: TTBR1 maps kernel upper half, PC is now virtual
//   - Returns to boot.S, which jumps to kernel_main at upper-half VA
// Notes:
//   - Called from boot.S in PAS (physical). Returns with MMU enabled.
//   - PC-relative BL from physical _start to physical early_init works even
//     when early_init is linked at upper-half VA because both are mapped
//     (identity at low PA, upper-half at high VA). Boot.S jumps directly
//     to physical addresses before MMU is live.
```

#### kernel_main() — Second-stage kernel initialization (VAS upper half)
```c
void kernel_main(void);
// Preconditions:
//   - Called from boot.S with MMU LIVE (TTBR1 mapped)
//   - CPU is executing at upper-half VAs
//   - BSS, UART, exceptions, PMM, MMU, and early subsystems initialized
// Actions:
//   1. mmio_switch_to_upper() — relocate MMIO base addresses to upper half
//   2. exceptions_init_upper() — relocate VBAR_EL1 to upper-half VA
//   3. pmm_relocate_upper() — move PMM bitmap to upper-half accessible region
//   4. cpu_init() — snapshot MIDR/CTR/feature regs, enable PMU cycle counter
//   5. heap_init() — initialize kernel heap allocator
//   6. gic_init() — initialize GICv3 interrupt controller
//   7. pci_enumerate_bus() — discover PCI devices
//   8. pci_virtio_rng_init() — initialize Virtio RNG
//   9. pci_virtio_blk_init() — initialize Virtio block device
//   10. pci_virtio_net_init() — initialize Virtio network device
//   11. pci_virtio_balloon_init() — initialize Virtio balloon
//   12. pci_virtio_console_init() — initialize Virtio console
//   13. fat32_mount() — mount FAT32 filesystem from block device
//   14. vfs_init() — initialize virtual filesystem
//   15. devices_register() — mount /dev/console, /dev/null, /dev/zero, /dev/rng
//   16. vfs_create_node() — create /mnt and /mnt/fat32 mount points
//   17. fat32_vfs_mount("/mnt/fat32") — mount FAT32 at path
//   18. proc_init() — initialize /proc filesystem
//   19. sched_init() — initialize scheduler and task structures
//   20. sched_create_task() — spawn user tasks (task_a, task_b, task_shell, task_crash)
//   21. sched_create_kernel_task() — spawn kernel daemon (netd)
//   22. timer_init() — initialize generic timer (CNTP)
//   23. timer_start(TIMER_INTERVAL_MS) — begin periodic ticks (10 ms)
//   24. Enter idle loop: while(1) wfi (wait for interrupt)
// Postconditions:
//   - All subsystems initialized
//   - Tasks spawned and runnable
//   - Timer ticking, interrupts enabled
//   - CPU waits for interrupts
// Notes:
//   - Never returns. If it does, kernel_panic_return() halts.
//   - All VAs are upper-half (0xFFFF_0000_0000_0000 + PA).
```

#### zero_bss() — Inline in kernel.c
```c
static void zero_bss(void);
// Preconditions:
//   - __bss_start and __bss_end symbols defined by linker script
// Actions:
//   - memset([__bss_start, __bss_end), 0, size)
// Postconditions:
//   - All uninitialized data is zeroed (BSS = .bss segment)
```

#### enable_fp_simd() — Inline in kernel.c
```c
static void enable_fp_simd(void);
// Preconditions:
//   - In EL1 with CPACR_EL1 writable
// Actions:
//   - MRS CPACR_EL1 into x0
//   - Set bits [21:20] = 0b11 (FPEN, enable FP/SIMD in EL1)
//   - MSR x0 back to CPACR_EL1
//   - ISB (instruction synchronization barrier)
// Postconditions:
//   - FP/SIMD instructions are now usable in EL1
//   - GCC can use SIMD registers in function prologues (e.g., varargs)
// Notes:
//   - Failure to set FPEN causes ESR_EL1 = 0x1FE00000 (undefined instruction)
//     when GCC tries to use SIMD registers for register parameters.
```

#### print_current_el() — From lib/utils/utils.c
```c
void print_current_el(void);
// Preconditions:
//   - UART initialized
// Actions:
//   - MRS CurrentEL into a temp register
//   - Extract bits [3:2] (exception level 0-3)
//   - Look up level name (EL0="User Space", EL1="Kernel Space", EL2="Hyper Space", EL3="Secure Monitor/Firmware")
//   - uart_printf() the result
// Postconditions:
//   - Prints "Current Exception Level: <name>" to UART
// Example output: "Current Exception Level: Kernel Space"
```

### From lib/uart/uart.c

#### uart_init()
```c
void uart_init(void);
// Initializes PL011 UART (0x09000000) to 115200 baud, 8N1.
// Baudrate calculation: divisor = clk / (16 * baud) = 24MHz / (16 * 115200) = 13.02083
//   Integer part (UART_IBRD) = 13
//   Fractional part (UART_FBRD) = round(0.02083 * 64) = 2
// Preconditions:
//   - Physical address 0x09000000 is mapped (identity or otherwise accessible)
// Postconditions:
//   - UART_CR[0] = 1 (UART enabled)
//   - UART_CR[8] = 1 (RX enabled)
//   - UART_CR[9] = 1 (TX enabled)
//   - FIFO, 8-bit, 1 stop bit, no parity configured
//   - uart_println() works
```

#### uart_puts(const char *str), uart_println(const char *str)
```c
void uart_puts(const char *str);       // Write NUL-terminated string
void uart_println(const char *str);    // Write string + newline
```

#### uart_printf(const char *fmt, ...)
```c
void uart_printf(const char *fmt, ...);
// Format specifiers: %s (const char*), %d (int64_t), %u (uint64_t),
//   %x (hex with 0x prefix), %p (pointer, same as %x),
//   %b (binary with 0b prefix), %c (char), %% (literal '%')
```

### From lib/panic/panic.c
```c
__attribute__((noreturn)) void kernel_panic(const char *msg);
// Dump exception state and halt CPU.
// Preconditions:
//   - Unrecoverable error detected
// Actions:
//   - Print "KERNEL PANIC: <msg>"
//   - Dump trap frame (if available)
//   - Halt: __asm__ volatile("hlt")
// Postconditions:
//   - CPU stops; never returns
```

## Rust Porting Strategy

### Module Structure
```rust
// src/boot.rs
//   - BootContext: zero-sized marker for "before MMU"
//   - UpperHalf: zero-sized marker for "after MMU"
//   - Functions to match C signatures:
//     - pub extern "C" fn early_init() (called from asm)
//     - pub extern "C" fn kernel_main() (called from asm)
//     - pub fn zero_bss()
//     - pub fn enable_fp_simd()
//     - pub fn print_current_el()

// src/arch/aarch64/boot.S (kept as assembly)
//   - _start entry point
//   - BSS setup
//   - Physical stack setup
//   - BL to early_init
//   - MMU enable
//   - Relocation to upper half
//   - BR to kernel_main

// src/arch/aarch64/sysregs.rs
//   - Wrapper functions for MRS/MSR:
//     - fn read_cpacr_el1() -> u64
//     - fn write_cpacr_el1(val: u64)
//     - fn read_currentel() -> u8
//     - fn read_pmccntr_el0() -> u64
//     - etc.
//   - Inline assembly using core::arch::aarch64 or custom asm! blocks

// src/mm/mod.rs
//   - Depend on early_init having called pmm_init, mmu_init
//   - pub fn phys_to_virt(pa: u64) -> u64
//   - pub fn virt_to_phys(va: u64) -> u64

// src/interrupts/mod.rs
//   - Trap frame layout matching exception.h
//   - handlers for EC_* exception classes
//   - Called from asm vector.S
```

### Type Strategy
- **Sysregs**: Each as a newtype or associated function module.
  ```rust
  pub mod sysregs {
      pub fn read_currentel() -> u8 { /* MRS */ }
      pub fn write_cpacr_el1(val: u64) { /* MSR */ }
  }
  ```
- **Boot state**: Use marker types or `PhantomData` to track phases (pre-MMU, post-MMU).
- **Linker symbols**: Use `extern "C"` to import `__bss_start`, `__bss_end`.

### Static & Locking
- **Early boot (single-threaded, before MMU)**: No locks needed; use unsafe functions or static mut with comments.
- **After MMU (multithreaded)**: Use `Mutex` or spinlocks for shared state (PMM, heap, interrupt handlers).
- **Per-CPU state**: Thread-local equivalent or per-CPU statics.

### Inline Assembly
- Use `core::arch::aarch64` for MRS/MSR, or define custom `asm!` blocks for clarity.
- Example:
  ```rust
  pub fn read_currentel() -> u8 {
      let el: u64;
      unsafe {
          asm!("mrs {}, CurrentEL", out(reg) el);
      }
      ((el >> 2) & 0b11) as u8
  }
  ```

### Assembly Portions That Must Stay in ASM
1. **boot.S _start**:
   - Secondary CPU park (MPIDR checks, spinlock)
   - Physical stack setup (depends on exact layout before _start runs)
   - MMU enable sequence (sysreg setup, barriers, ISB, TLB invalidate)
   - Relocation to upper half (depends on linker layout, PC-relative addressing)

2. **vector.S exception entry**:
   - Trap frame save/restore (must match C struct layout)
   - ELR/SPSR/ESR/FAR capture (sysregs)
   - Exception number routing to handlers

3. **context switch (sched context)**:
   - TTBR0 switch with ASID
   - X0-X30 restore from task_t (offset-based, hard-coded in asm)
   - ELR restoration and ERET

### Gotchas & Correctness Pitfalls

1. **KERNEL_VA_OFFSET must be exact**: 0xFFFF_0000_0000_0000. Any typo breaks all upper-half addresses.
2. **Linker symbols (__bss_start, __bss_end)**: Must be defined by linker.ld; zero_bss must not read them too early (before linker.ld applied).
3. **Physical vs. Virtual**: Before MMU, use PAs; after MMU, use VAs. Mixing causes immediate crash or infinite TLB faults.
4. **ISB after sysreg writes**: `isb` after writes to CPACR_EL1, TTBR*, TCR_EL1, etc., to ensure synchronization.
5. **DSB before ISB**: For memory-mapped I/O (MMIO), `dsb sy` before MMIO reads/writes to ensure ordering.
6. **TTBR0 ASID bits**: When writing TTBR0, pack ASID into bits [63:48] or it gets zeroed, breaking TLB tagging.
7. **PTE Address Mask**: PTEs encode PA in bits [47:12] (4 KB aligned). Misaligned addresses silently corrupt page tables.
8. **AP vs. PTE_AP**: Don't confuse AP[2:1] (permission bits at [7:6]) with EL0 RW flags; check ARM ARM carefully.
9. **Stage-2 vs. Stage-1 faults**: Early kernel runs in stage-1 translation only (no hypervisor); ESR_ISS_S1PTW would indicate a bug.
10. **Inline asm volatile**: Use `core::arch::aarch64::__dsb` / `__isb` or explicit asm! volatile to prevent reordering.

## Boot / Usage Ordering

1. **Power-on**: CPU fetches first instruction from address 0 (or device-dependent reset vector). QEMU virt: 0x40000000.
2. **_start (boot.S)**:
   - Park secondary CPUs (spin on MPIDR checks).
   - Load `__stack_top` VA (linked upper-half).
   - Subtract `KERNEL_VA_OFFSET` to get physical address.
   - Set SP to physical stack top.
   - BL early_init (PC-relative, works even though early_init is linked upper-half because the physical address of the code is still reachable via PC-relative).
3. **early_init() (kernel.c, in PAS)**:
   - zero_bss(), enable_fp_simd(), uart_init(), print_current_el()
   - exceptions_init() (install VBAR_EL1 to physical address of vector table)
   - pmm_init(), mmu_init() (build page tables with TTBR1)
   - Enable MMU: set TCR_EL1, MAIR_EL1, TTBR1_EL1, then SCTLR_EL1.M = 1, ISB, DSB
   - From this point, PC is virtual
   - Return to boot.S
4. **boot.S continuation** (now with MMU live):
   - Load `__stack_top` VA (now interpreted as virtual address by MMU)
   - Move to SP
   - Load kernel_main VA
   - BR kernel_main (branch register, absolute address — MMU resolves)
5. **kernel_main() (kernel.c, in VAS upper half)**:
   - mmio_switch_to_upper(), exceptions_init_upper(), pmm_relocate_upper()
   - Initialize all remaining subsystems
   - Spawn tasks
   - Enter idle loop (wfi)
6. **Interrupt/Exception**: CPU branches to vector table (VBAR_EL1), trap frame saved, handler called, ERET resumes.

## Summary Table

| Stage | PC | VA | Subsystems Up | Notes |
|-------|----|----|----------------|-------|
| Power-on to _start | Physical | Identity | None | CPU boots at PA 0x40000000 (QEMU virt) |
| boot.S _start | Physical | Identity | None | Secondary CPU park, physical stack setup |
| early_init() | Physical | Identity | UART, exceptions (VBAR physical), PMM, MMU tables | Builds and enables MMU; returns with MMU live |
| boot.S post-MMU | Virtual (upper-half) | Upper-half | UART, exceptions (VBAR now upper-half), PMM, MMU | Stack and kernel_main in upper half |
| kernel_main() | Virtual (upper-half) | Upper-half | All: UART, exceptions, PMM, MMU, CPU, heap, GIC, PCI, VirtIO, FS, tasks, timer | Enters idle loop |

