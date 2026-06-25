# Exception Handling Subsystem (ARMv8 AArch64)

## Overview

The exception subsystem implements ARMv8-A AArch64 trap frame capture, exception classification, routing, and dispatch for Fermi OS. It handles:

- **Synchronous exceptions**: SVC (syscalls), data/instruction aborts, alignment faults, breakpoints, and other traps
- **Asynchronous interrupts**: IRQs and FIQs via the GICv3 distributor and redistributor
- **SErrors**: Asynchronous external aborts (fatal)
- **EL0↔EL1 boundary**: Separate vector entries for kernel-to-kernel (EL1→EL1) and user-to-kernel (EL0→EL1) transitions with SP_EL0 preservation
- **Fault diagnostics**: DFSC decoding, FAR-based address classification, multi-task ASID tracking
- **Preemption support**: Caller-clobbered FP/SIMD register preservation (q0-q7, q16-q31, FPSR, FPCR)

The vector table is installed twice: once at physical address during early boot (before MMU), and again at upper-half virtual address after MMU enable.

## Public API

### Data Types

#### `trap_frame_t` (Exception Context)

```c
typedef struct trap_frame {
  uint64_t regs[31];     // x0-x30 (general-purpose registers)
  uint64_t elr;          // exception link register (return address)
  uint64_t spsr;         // saved processor state register
  uint64_t esr;          // exception syndrome register
  uint64_t far;          // fault address register
} trap_frame_t;
```

**Size**: 5 fields × 8 bytes = 40 bytes (C struct, not including frame overhead)

**Layout in vector.S trap frame** (see below for full 688-byte frame):
- `[sp + 0]`:   x0-x30 (31 × 8 = 248 bytes)
- `[sp + 248]`: ELR_EL1
- `[sp + 256]`: SPSR_EL1
- `[sp + 264]`: ESR_EL1
- `[sp + 272]`: FAR_EL1
- `[sp + 280]`: SP_EL0 (saved on EL0 exceptions only)
- `[sp + 288]`: q0-q31 caller-clobbered SIMD state (16 × 16 = 256 bytes, q0-q7 at 288, q16-q31 at 416)
- `[sp + 672]`: FPSR
- `[sp + 680]`: FPCR
- **Total**: 688 bytes (16-byte aligned)

### Functions

#### `void exceptions_init(void)`

**Called during**: Early boot (`early_init`), before MMU enable  
**Purpose**: Install the vector table at its physical address  
**Implementation**:
1. Read symbol `&vector_table` (PC-relative → physical at this stage)
2. Write to `VBAR_EL1` via inline `__asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar))`
3. Issue `isb` (instruction synchronization barrier)

**Note**: The vector table base must be 2048-byte aligned per ARM VBAR_EL1 specification.

#### `void exceptions_init_upper(void)`

**Called during**: After MMU enable (`upper_half_init` equivalent)  
**Purpose**: Relocate vector table reference to upper-half virtual address  
**Implementation**:
1. Read symbol `&vector_table` (now PC-relative → upper-half VA due to `-fno-pic` linking)
2. Write updated address to `VBAR_EL1`
3. Issue `isb`

**Note**: The physical and virtual addresses are the same numeric value (due to the KERNEL_VA_OFFSET offset), but the semantics change: the CPU now interprets it as a VA rather than PA.

#### `void exception_dispatch(uint64_t type, trap_frame_t *frame)`

**Called from**: `exception_common` (assembly), passing exception type and SP (trap frame pointer)  
**Purpose**: Route exceptions by type and class to appropriate handler  
**Parameters**:
- `type`: One of `EXCEPTION_SYNC`, `EXCEPTION_IRQ`, `EXCEPTION_FIQ`, `EXCEPTION_SERROR`
- `frame`: Pointer to trap frame on the exception handler stack

**Dispatch logic**:

##### Synchronous Exceptions (type = EXCEPTION_SYNC)

Dispatches based on Exception Class (EC) from `ESR_EL1[31:26]`:

| EC | Name | Value | Action |
|---|---|---|---|
| `EC_SVC_AARCH64` | SVC (syscall) | 0x15 | Call `syscall_dispatch(frame)` |
| `EC_DATA_ABORT_CUR` | Data abort (kernel) | 0x25 | Dump frame, panic |
| `EC_DATA_ABORT_LO` | Data abort (user) | 0x24 | Try `sched_try_grow_stack()` for translation faults L1/L2/L3; else kill task |
| `EC_INST_ABORT_CUR` | Instruction abort (kernel) | 0x21 | Dump frame, panic |
| `EC_INST_ABORT_LO` | Instruction abort (user) | 0x20 | Dump diagnosis, kill task |
| `EC_BRK` | Breakpoint | 0x3C | Dump frame, increment ELR by 4, continue |
| (others) | Unknown | — | Dump frame, panic |

**Data abort handling (user)**:
- Extract DFSC from `ESR_EL1[5:0]` via `ESR_ISS_DFSC(esr)`
- If DFSC ∈ {0x05, 0x06, 0x07} (translation fault L1/L2/L3) within user stack growth zone:
  - Call `sched_try_grow_stack(current_task, far)` to demand-page a new stack page
  - On success: return (stack unwound, resumes faulting instruction via `eret`)
  - On failure: fall through to kill
- On kill: call `dump_user_abort()`, then `task_exit()`

##### IRQ (type = EXCEPTION_IRQ)

1. Call `gic_ack_irq()` to get the interrupt ID
2. If no pending (ID = 1023), return
3. Call `gic_count_irq(intid)` for statistics
4. If `intid == TIMER_PPI_INTID` (30), call `timer_handle_irq()`
5. Otherwise, log unimplemented interrupt
6. Call `gic_end_irq(intid)` to signal EOI
7. Call `schedule()` to enable preemption

##### FIQ (type = EXCEPTION_FIQ)

Dump frame and panic (not yet supported).

##### SError (type = EXCEPTION_SERROR)

Dump frame and panic (asynchronous abort).

### Diagnostic Functions (Static, Internal to exception.c)

These are exported implicitly via symbol table but primarily for post-mortem debugging:

#### `dump_user_abort(what, task, frame)`

**Purpose**: Compact user-fault diagnostic dump  
**Output** (via UART):
```
[FAULT] <what> in task <pid> '<name>' ASID=<asid>
  ELR=<hex>  FAR=<hex>  ESR=<hex>  SPSR=<hex>
  cause: <dfsc_str> (DFSC=<hex>)  [write|read]  [cache-maint]  [external-abort]
  FAR region: <classification>
  -> killing task
```

**FAR address classification logic** (`va_classify_user`):
- `far >= 0xFFFF000000000000`: "kernel-half VA (kernel-pointer leak?)"
- `far < 0x1000`: "NULL-page (nullptr deref)"
- `far ∈ [USER_TEXT_BASE, USER_STACK_TOP - USER_STACK_PAGES × 0x1000)`: "user code / data region"
- `far ∈ [USER_STACK_TOP - USER_STACK_PAGES × 0x1000, USER_STACK_TOP)`: "user stack (active)"
- `far ∈ [USER_STACK_TOP - (USER_STACK_PAGES + 1) × 0x1000, USER_STACK_TOP - USER_STACK_PAGES × 0x1000)`: "just below user stack — STACK OVERFLOW likely"
- `far >= USER_STACK_TOP`: "above user range — wild pointer"
- Otherwise: "user lower-half (unmapped)"

#### `dump_trap_frame(type, frame)`

**Purpose**: Full trap frame hexdump (verbose)  
**Output**:
```
========== EXCEPTION ==========
  Type : <type_str>
  Class: <ec_str> (EC=<hex>)
  ESR_EL1 : <hex>
  ELR_EL1 : <hex>
  FAR_EL1 : <hex>
  SPSR_EL1 : <hex>
  Registers:
    x0 = <hex>
    x1 = <hex>
    ...
    x30 = <hex>
===============================
```

## Constants and Register Definitions

### Exception Types (vector table groups)

```c
#define EXCEPTION_SYNC    0
#define EXCEPTION_IRQ     1
#define EXCEPTION_FIQ     2
#define EXCEPTION_SERROR  3
```

### ESR_EL1 (Exception Syndrome Register) Decoding

**Exception Class (EC)** — bits [31:26]:

```c
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK (0x3FULL << ESR_EC_SHIFT)
#define ESR_EC(esr) (((esr) >> ESR_EC_SHIFT) & 0x3F)
```

**Exception classes**:

```c
#define EC_UNKNOWN          0x00  // Unknown reason
#define EC_WF_TRAPPED       0x01  // WFI/WFE trapped
#define EC_SVC_AARCH64      0x15  // SVC (syscall, AArch64)
#define EC_HVC_AARCH64      0x16  // HVC (hypervisor call, AArch64)
#define EC_SMC_AARCH64      0x17  // SMC (secure call, AArch64)
#define EC_INST_ABORT_LO    0x20  // Instruction abort from lower EL
#define EC_INST_ABORT_CUR   0x21  // Instruction abort from current EL
#define EC_PC_ALIGN         0x22  // PC alignment fault
#define EC_DATA_ABORT_LO    0x24  // Data abort from lower EL
#define EC_DATA_ABORT_CUR   0x25  // Data abort from current EL
#define EC_SP_ALIGN         0x26  // SP alignment fault
#define EC_FP_AARCH64       0x2C  // Floating-point exception (AArch64)
#define EC_SERROR           0x2F  // SError interrupt
#define EC_BRK              0x3C  // Breakpoint (BRK instruction)
```

**Instruction/Data Abort ISS Layout** — when EC ∈ {0x20, 0x21, 0x24, 0x25}:

```c
#define ESR_ISS_DFSC(esr)   ((esr) & 0x3F)         // bits [5:0]   Data/Instr Fault Status Code
#define ESR_ISS_WNR(esr)    (((esr) >> 6) & 0x1)   // bit  [6]     Write-Not-Read (data abort only)
#define ESR_ISS_S1PTW(esr)  (((esr) >> 7) & 0x1)   // bit  [7]     Stage-1 Page Table Walk fault
#define ESR_ISS_CM(esr)     (((esr) >> 8) & 0x1)   // bit  [8]     Cache maintenance
#define ESR_ISS_EA(esr)     (((esr) >> 9) & 0x1)   // bit  [9]     External Abort
```

### Data/Instruction Fault Status Codes (DFSC)

**Per ARM ARM DDI 0487, Table D13-9.** The DFSC value indicates the level of page-table walk where the fault occurred and the fault type:

```
Address Size Faults (illegal page table entry):
  0x00: Address size fault @ L0 / TTBR
  0x01: Address size fault @ L1
  0x02: Address size fault @ L2
  0x03: Address size fault @ L3

Translation Faults (unmapped, no valid PTE):
  0x04: Translation fault @ L0
  0x05: Translation fault @ L1
  0x06: Translation fault @ L2
  0x07: Translation fault @ L3

Access Flag Faults (AF=0 in PTE):
  0x09: Access flag fault @ L1
  0x0A: Access flag fault @ L2
  0x0B: Access flag fault @ L3

Permission Faults (AP bits deny access):
  0x0D: Permission fault @ L1
  0x0E: Permission fault @ L2
  0x0F: Permission fault @ L3

External Aborts:
  0x10: Synchronous external abort (not during page-table walk)
  0x14: Synchronous external abort during TT walk @ L0
  0x15: Synchronous external abort during TT walk @ L1
  0x16: Synchronous external abort during TT walk @ L2
  0x17: Synchronous external abort during TT walk @ L3

Miscellaneous:
  0x21: Alignment fault
  0x30: TLB conflict abort

Other values: Unknown DFSC
```

### Hardware Constants (System Registers)

All read/written via inline `__asm__ __volatile__("mrs/msr reg, <sysreg>")`:

| Register | Purpose | Used In |
|----------|---------|---------|
| `VBAR_EL1` | Vector Base Address (must be 2048-byte aligned) | `exceptions_init*` |
| `ELR_EL1` | Exception Link Register (return address) | saved/restored in trap frame |
| `SPSR_EL1` | Saved Processor State Register | saved/restored in trap frame; mode check @ `[sp+256]` for EL0 return |
| `ESR_EL1` | Exception Syndrome Register | decoded for EC, ISS fields |
| `FAR_EL1` | Fault Address Register | decoded for address classification |
| `SP_EL0` | User stack pointer (saved on EL0 exceptions) | saved @ `[sp+280]`, restored if returning to EL0 |
| `FPSR` | FP Status Register | saved/restored for SIMD preservation |
| `FPCR` | FP Control Register | saved/restored for SIMD preservation |

### User Address Space Layout

```c
#define USER_TEXT_BASE       0x00400000ULL  // User code start (4 MB)
#define USER_STACK_TOP       0x00800000ULL  // User stack top (8 MB)
#define USER_STACK_PAGES     4              // Initial user stack (16 KiB)
#define USER_STACK_PAGES_MAX 64             // Max demand-paged user stack (256 KiB total)
#define USER_STACK_GROWN_MAX (USER_STACK_PAGES_MAX - USER_STACK_PAGES)
```

### GIC Constants

```c
#define GIC_INTID_NO_PENDING 1023  // Spurious interrupt from GIC
```

### Timer Constants

```c
#define TIMER_PPI_INTID 30  // Private Peripheral Interrupt ID for generic timer
```

## Vector Table Layout and Assembly Entry Points

**Base address**: `extern char vector_table[]` (symbol provided by linker script)  
**Alignment**: 2048 bytes (per ARM VBAR_EL1 spec)  
**Size**: 512 bytes (4 groups × 4 entries × 32 bytes per entry)

Each entry is 128 bytes (16 instructions, `.balign 128`), organized in 4 groups of 4 entries:

```
Offset   Group     Entry                  Macro Used
------   -----     -----                  -----------
0x000    Group 0   Current EL, SP_EL0    VECTOR_ENTRY_INVALID
0x080                (SYNC)
0x100                (IRQ)
0x180                (FIQ)
0x200    [reserved for padding]

0x200    Group 1   Current EL, SP_ELx    VECTOR_ENTRY
0x280                (SYNC) - kernel→kernel
0x300                (IRQ)
0x380                (FIQ)
0x400    [reserved for padding]

0x400    Group 2   Lower EL, AArch64     VECTOR_ENTRY_LOWER
0x480                (SYNC) - user→kernel
0x500                (IRQ)
0x580                (FIQ)
0x600    [reserved for padding]

0x600    Group 3   Lower EL, AArch32     VECTOR_ENTRY_INVALID
0x680                (SYNC) [unused]
0x700                (IRQ)
0x780                (FIQ)
0x800    [reserved for padding]
```

**Group 0 (Current EL with SP_EL0)**: Marked `VECTOR_ENTRY_INVALID` because Fermi runs EL1 with SP_ELx (not SP_EL0). Should never fire; indicates a hypervisor or processor bug if triggered.

**Group 1 (Kernel-to-Kernel)**: Kernel exception handlers (current implementation handles only synchronous exceptions and IRQs at this level).

**Group 2 (User-to-Kernel)**: User-space exceptions. Saves SP_EL0 into trap frame.

**Group 3 (AArch32 Lower EL)**: Unused; Fermi does not support AArch32 user processes.

### Vector Entry Macros and exception_common

#### `VECTOR_ENTRY_LOWER` (Group 2 entries)

```asm
.macro VECTOR_ENTRY_LOWER type
.balign 128
	sub sp, sp, #FRAME_SIZE        // Allocate 688-byte trap frame
	stp x0, x1, [sp, #0]           // Save x0, x1 at frame start
	mrs x0, sp_el0                 // Read user stack pointer
	str x0, [sp, #280]             // Save SP_EL0 in frame
	mov x0, #\type                 // Load exception type
	b exception_common             // Jump to common handler
.endm
```

The type argument (0-3) passed to `exception_common` distinguishes SYNC/IRQ/FIQ/SERROR at dispatch time.

#### `VECTOR_ENTRY` (Group 1 entries)

```asm
.macro VECTOR_ENTRY type
.balign 128
	sub sp, sp, #FRAME_SIZE
	stp x0, x1, [sp, #0]
	mov x0, #\type
	b exception_common
.endm
```

Does not save SP_EL0 (running at EL1 already).

#### `exception_common` (Assembly handler, full register/SIMD save)

**Responsibilities**:
1. Save all 31 GPRs (x0-x30)
2. Save system registers (ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1)
3. Save caller-clobbered SIMD state (q0-q7, q16-q31, FPSR, FPCR)
4. Call C handler `exception_dispatch(type, frame_ptr)`
5. On return, restore ELR_EL1, SPSR_EL1, all GPRs, SIMD state
6. Conditionally restore SP_EL0 if returning to EL0 (check SPSR_EL1.M[3:0] == 0)
7. Deallocate trap frame
8. Execute `eret` (return from exception)

**Key invariants**:
- x0 (exception type) is pre-loaded by vector entry macro before jumping to `exception_common`
- x0/x1 are already saved at `[sp + 0]` by vector entry
- FP/SIMD preservation is essential because C handlers (e.g., `uart_printf`) may use SIMD for varargs

**SPSR_EL1 mode check for EL0 return**:

```asm
and x2, x1, #0xF        // Extract SPSR.M[3:0] (execution mode bits)
cbnz x2, 1f             // If non-zero, we're returning to EL1; skip SP_EL0 restore
ldr x1, [sp, #280]      // Load saved SP_EL0
msr sp_el0, x1          // Restore SP_EL0
1:                      // (label for branch target)
```

If mode bits are 0, we're returning to EL0; restore SP_EL0. Otherwise, leave it alone.

## Trap Frame Layout in Memory

**Total size**: 688 bytes (16-byte aligned)

```
Offset (bytes)  Field               Size (bytes)
------          -----               ----
0               x0                  8
8               x1                  8
16              x2                  8
24              x3                  8
32              x4                  8
40              x5                  8
48              x6                  8
56              x7                  8
64              x8                  8
72              x9                  8
80              x10                 8
88              x11                 8
96              x12                 8
104             x13                 8
112             x14                 8
120             x15                 8
128             x16                 8
136             x17                 8
144             x18                 8
152             x19                 8
160             x20                 8
168             x21                 8
176             x22                 8
184             x23                 8
192             x24                 8
200             x25                 8
208             x26                 8
216             x27                 8
224             x28                 8
232             x29                 8
240             x30 (LR)            8
248             ELR_EL1             8
256             SPSR_EL1            8
264             ESR_EL1             8
272             FAR_EL1             8
280             SP_EL0              8   (EL0 exceptions only; EL1→EL1 undefined)
288             q0                  16
304             q1                  16
320             q2                  16
336             q3                  16
352             q4                  16
368             q5                  16
384             q6                  16
400             q7                  16
416             q16                 16
432             q17                 16
448             q18                 16
464             q19                 16
480             q20                 16
496             q21                 16
512             q22                 16
528             q23                 16
544             q24                 16
560             q25                 16
576             q26                 16
592             q27                 16
608             q28                 16
624             q29                 16
640             q30                 16
656             q31                 16
672             FPSR                8
680             FPCR                8
688             [END - 16-byte boundary]
```

**Why q8-q15 are not saved**: Per AAPCS64, q8-q15 are callee-saved. The context_switch routine (in sched subsystem) preserves them across task switches, so no need to save in trap frame.

**Why FP regs are saved in trap frame**: On preemption (timer IRQ at EL0), the exception handler C code (e.g., uart_printf) may use SIMD for varargs passing. Without saving q0-q7 and q16-q31 here, the preempted user task resumes with corrupted FP state.

## Boot and Initialization Sequence

1. **Early boot** (`early_init` in kernel.c):
   - BSS zeroed
   - FP/SIMD enabled (CPACR_EL1.FPEN = 0b11)
   - UART initialized
   - `exceptions_init()` called → vector table installed at physical address
   - PMM initialized
   - MMU initialized, `mmu_run_tests()` called
   - Print message, prepare to jump to upper half

2. **After MMU enable** (upper-half init):
   - `exceptions_init_upper()` called → vector table address re-read (now VA instead of PA)
   - Vector table is now at upper-half virtual address per KERNEL_VA_OFFSET

## Exception Dispatch Flow

```
[Vector entry (Group 1 or 2)]
  → VECTOR_ENTRY or VECTOR_ENTRY_LOWER macro
  → exception_common (asm)
    → Save all registers, SIMD, system regs
    → Call exception_dispatch(type, frame)
      [C handler]
      ├─ If SYNC:
      │   └─ Decode EC, dispatch:
      │       ├─ SVC_AARCH64 → syscall_dispatch(frame)
      │       ├─ DATA_ABORT_LO → sched_try_grow_stack() or kill task
      │       ├─ INST_ABORT_LO → dump_user_abort(), kill task
      │       ├─ DATA/INST_ABORT_CUR → panic
      │       ├─ BRK → increment ELR, continue
      │       └─ (others) → panic
      ├─ If IRQ:
      │   └─ gic_ack_irq() → if TIMER_PPI_INTID: timer_handle_irq() → schedule()
      ├─ If FIQ:
      │   └─ panic
      └─ If SERROR:
          └─ panic
    → Restore all registers, SIMD, system regs
    → Conditionally restore SP_EL0 (if EL0 return)
    → eret (resume faulting instruction or next instruction)
```

## Cross-Subsystem Dependencies

**exception.c calls**:
- `uart_printf`, `uart_println`: Logging
- `syscall_dispatch(frame)`: Syscall routing
- `sched_current()`: Get current task
- `sched_try_grow_stack(task, far)`: Demand-page user stack
- `task_exit()`: Kill task
- `gic_ack_irq()`, `gic_count_irq()`, `gic_end_irq()`: Interrupt acknowledgement
- `timer_handle_irq()`: Timer IRQ handler
- `schedule()`: Preemption scheduling
- `kernel_panic()`: Fatal error

**exception.h/vector.S accessed by**:
- `kernel.c`: Includes exception.h, calls `exceptions_init()` and `exceptions_init_upper()`
- Linker script: Provides `vector_table` symbol; places it at 2048-byte boundary

## Gotchas and Subtle Correctness Issues

1. **SPSR_EL1 mode check**: Must extract bits [3:0] to determine EL0 return (value 0 means EL0). Failure to check causes SP_EL0 to remain stale, breaking user-space stack pointer on resume.

2. **16-byte SIMD offset arithmetic**: All `stp q,q,[sp,#imm]` use scaled offsets (×16 bytes). Offsets must fit signed 7-bit range (−1008 to +1008 bytes). The frame size of 688 bytes is chosen to keep all SIMD stores in range.

3. **SP_EL0 only on EL0 exceptions**: VECTOR_ENTRY_LOWER saves SP_EL0 into the frame. VECTOR_ENTRY does not. On kernel-to-kernel exception, the frame's SP_EL0 slot is undefined; do not use it.

4. **Demand-paged stack growth window**: Only translation faults (DFSC ∈ {0x05, 0x06, 0x07}) in the mapped-but-not-yet-used region trigger sched_try_grow_stack(). Permission faults, address-size faults, and other DFSC values cause immediate task termination.

5. **Vector table alignment and relocation**: The vector table must remain at a 2048-byte-aligned address through the MMU transition. If the linker moves the vector table across the MMU boundary, VBAR_EL1 must be updated. The code handles this by re-reading &vector_table symbol post-MMU.

6. **ASID in TTBR0[63:48]**: The dump_user_abort function reads the live ASID from TTBR0_EL1 to correlate faults with tasks. If ASID allocation logic changes, update the read.

7. **Caller-clobbered FP register preservation**: The AAPCS64 ABI marks q0-q7 and q16-q31 as caller-clobbered; q8-q15 are callee-saved. Saving the wrong set leads to data corruption if a preempted task relied on q8-q15 values.

8. **Inline assembly volatility**: All reads/writes to system registers must use `__asm__ __volatile__(...)` to prevent compiler reordering. Failure allows stale register values to be used.

9. **ISB after VBAR_EL1 write**: The `isb` instruction after `msr vbar_el1` is mandatory to ensure subsequent fetches use the new vector table base.

10. **DFSC level decoding**: DFSC values for address-size and translation faults encode the page-table level (L0-L3). A user-space pointer that faults at L0 strongly suggests a kernel-pointer dereference (because user TTBR0 is small); L3 faults are typical unmapped-page faults.

## Rust Port Strategy

### Module Structure

```rust
// src/exception/mod.rs
pub mod trap_frame;
pub mod vector;
pub mod constants;
pub mod dispatch;

pub fn init_early();          // exceptions_init equivalent
pub fn init_upper();          // exceptions_init_upper equivalent
```

### Key Types and Statics

```rust
// trap_frame.rs
#[repr(C)]
pub struct TrapFrame {
    pub regs: [u64; 31],    // x0-x30
    pub elr: u64,           // ELR_EL1
    pub spsr: u64,          // SPSR_EL1
    pub esr: u64,           // ESR_EL1
    pub far: u64,           // FAR_EL1
}

// constants.rs
pub const EXCEPTION_SYNC: u64 = 0;
pub const EXCEPTION_IRQ: u64 = 1;
pub const EXCEPTION_FIQ: u64 = 2;
pub const EXCEPTION_SERROR: u64 = 3;

pub const ESR_EC_SHIFT: u32 = 26;
pub const ESR_EC_MASK: u64 = 0x3F << ESR_EC_SHIFT;

#[repr(u64)]
pub enum ExceptionClass {
    Unknown = 0x00,
    WfTrapped = 0x01,
    SvcAarch64 = 0x15,
    HvcAarch64 = 0x16,
    SmcAarch64 = 0x17,
    InstAbortLo = 0x20,
    InstAbortCur = 0x21,
    PcAlign = 0x22,
    DataAbortLo = 0x24,
    DataAbortCur = 0x25,
    SpAlign = 0x26,
    FpAarch64 = 0x2C,
    Serror = 0x2F,
    Brk = 0x3C,
}

// dispatch.rs
pub fn exception_dispatch(typ: u64, frame: &mut TrapFrame);

// Decoder functions
pub fn esr_ec(esr: u64) -> u64;
pub fn esr_iss_dfsc(esr: u64) -> u8;
pub fn esr_iss_wnr(esr: u64) -> bool;
pub fn esr_iss_cm(esr: u64) -> bool;
pub fn esr_iss_ea(esr: u64) -> bool;
pub fn esr_iss_s1ptw(esr: u64) -> bool;
```

### Static Data and Locking

The vector table itself is statically allocated in `.asm` or linker script and needs no runtime initialization beyond a single VBAR_EL1 write. No locks needed because:
- Vector table is read-only after boot
- VBAR_EL1 is a per-CPU register (no synchronization needed on single-CPU or IPI-coordinated multi-CPU)
- Exception dispatch is inherently single-threaded at any given CPU

The FAR classification and fault diagnostics operate on immutable data (task structures, frame contents), so no synchronization primitives are required.

### Assembly vs Rust

**Must remain in assembly** (inline asm or `.S` file):
1. Vector entry macros (VECTOR_ENTRY, VECTOR_ENTRY_LOWER, exception_common): these manipulate SP, save/restore system registers, and execute `eret`. Cannot be reliably expressed in Rust without `#[naked]` functions and inline asm, which defeats the point.
2. System register I/O: `msr vbar_el1`, `mrs elr_el1`, etc. Must use inline asm `__asm!("msr/mrs")` or a .S file.
3. `eret` instruction: Must be inline asm or `.S`.

**Can be ported to Rust**:
1. Exception class decoding (bitfield extraction)
2. Dispatch logic (match on exception type/class)
3. Fault diagnostics and formatting
4. Calls to subsystem handlers (syscall, GIC, timer, scheduler)
5. DFSC/SPSR decoding helper functions

### Ownership and Safety

- `trap_frame` is passed by mutable reference to exception_dispatch; the frame's ownership stays with the exception handler stack frame (assembly).
- Subsystem references (sched_current(), gic_ack_irq(), etc.) are obtained from stable global/static mutable state, protected by higher-level invariants (e.g., interrupt masking, single-CPU execution during early boot).
- Fault diagnostics read task structures but do not modify them (except task_exit(), which is a controlled subsystem call).

### Feature Usage

- `core::arch::asm!` for inline system register I/O
- No `alloc` or heap allocation in hot path
- Minimal dependencies on external crates (use kernel's own uart, sched, etc.)

---

## Reference Documents

- **ARM ARM (Architecture Reference Manual) DDI 0487**: Exception handling, system registers, ESR_EL1 layout, vector table, SPSR_EL1 format
- **AAPCS64 (Procedure Call Standard for AArch64)**: Caller/callee-saved register classifications, SIMD register usage
- **ARM CoreLink GIC-500 (GICv3) specification**: Interrupt acknowledgement, EOI, INTID layout
- **Fermi OS kernel.c**: Boot flow, MMU transition, subsystem initialization order

---

**Spec Version**: 1.0  
**Last Updated**: 2025-06-25  
**Status**: Implementation-ready for Rust port  

