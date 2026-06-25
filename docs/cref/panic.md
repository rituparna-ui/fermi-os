# Panic Subsystem Specification

## Overview

The panic subsystem is the kernel's emergency diagnostic and halt mechanism. When invoked via `kernel_panic(msg)`, it:

1. **Immediately masks all CPU exceptions** (Debug, Async-abort, IRQ, FIQ) to prevent re-entrance
2. **Captures and displays diagnostic state**: ELR_EL1, ESR_EL1, FAR_EL1, SPSR_EL1, SP, caller's LR
3. **Prints a formatted panic banner** and optional panic message to UART
4. **Permanently parks the CPU** in a `wfi` (Wait For Interrupt) loop with interrupts masked

This subsystem is intentionally minimal and must be usable from any CPU context (early boot, exception handler, kernel task) without heap allocation or complex locking.

---

## Public API

### Function: `kernel_panic`

```c
__attribute__((noreturn)) void kernel_panic(const char *msg);
```

**Signature Details:**
- **Parameter:** `msg` - optional panic reason string (may be NULL); printed if non-NULL
- **Return:** never returns; function never completes
- **Calling convention:** AAPCS64 (aarch64 ARM calling convention)
- **Attributes:** `__attribute__((noreturn))` — compiler knows this function never returns

**Behavior:**

1. **Interrupt masking (inline ASM):**
   - Execute: `msr daifset, #0xf`
   - Sets DAIF bits [3:0] = 1111 (mask Debug, Async-abort, IRQ, FIQ)
   - Clobber list: `"memory"` to prevent instruction reordering

2. **Caller LR capture (C builtin):**
   - Call `__builtin_return_address(0)` to read x30 before any C call can clobber it
   - Store result in `uint64_t caller_lr`
   - This is the instruction address **after** the `kernel_panic` call instruction

3. **UART output (in order):**
   ```
   (blank line)
   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
   !!!         KERNEL PANIC            !!!
   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
   (blank line)
   ```
   - Via `uart_println("")` (blank) and multiple `uart_println(banner_line)` calls

4. **Conditional panic message:**
   - If `msg != NULL`: print `"  Reason: " + msg + "\n"` via `uart_printf("  Reason: %s\n", msg)`

5. **System register capture (inline ASM, each with `volatile` and `"memory"` clobber):**
   - `mrs %0, elr_el1` → `elr` (Exception Link Register, points to faulting/resumable instruction)
   - `mrs %0, esr_el1` → `esr` (Exception Syndrome Register, encodes fault type/details)
   - `mrs %0, far_el1` → `far_reg` (Fault Address Register, VA of faulting access)
   - `mrs %0, spsr_el1` → `spsr` (Saved Processor State Register, CPU flags at exception)
   - `mov %0, sp` → `sp` (current stack pointer, SP_EL1 if from EL1, SP_EL0 if from EL0 user task)

6. **Diagnostic dump output (via `uart_printf`):**
   ```
   (blank line)
     ELR_EL1  (return addr) : <elr in hex>
     ESR_EL1  (syndrome)    : <esr in hex>
     FAR_EL1  (fault addr)  : <far_reg in hex>
     SPSR_EL1 (saved state) : <spsr in hex>
     SP       (stack ptr)   : <sp in hex>
     LR       (caller pc)   : <caller_lr in hex>
     
     System halted. Reset to continue.
   ```
   - All registers printed with `%x` (hexadecimal without 0x prefix, 64-bit value)
   - Single leading space for alignment

7. **CPU halt (infinite loop with `wfi`):**
   ```c
   while (1) {
     __asm__ __volatile__("wfi");
   }
   ```
   - `wfi` (Wait For Interrupt) puts CPU in low-power sleep
   - With DAIF[3:0] = 1111, **no interrupt will wake the CPU**
   - The only way to recover is a hardware reset (PSCI SYSTEM_RESET from another CPU, or external hard reset)

---

## Hardware Details & Constants

### ARM aarch64 System Registers (AAPCS64 Specification)

All reads are from the current exception level (EL1 for kernel, virtualized to current level if in hypervisor).

#### DAIF Register (Debug, Async Abort, IRQ, FIQ Interrupt Mask)
- **Address:** System register (MSR/MRS only)
- **Bitfield:** DAIF[3:0]
  - Bit 3: D (Debug exceptions masked)
  - Bit 2: A (Async-abort/SError masked)
  - Bit 1: I (IRQ masked)
  - Bit 0: F (FIQ masked)
- **Operation used:** `msr daifset, #0xf` — sets specified bits to 1 (mask)
  - Immediate `0xf` = 0b1111 masks all four exception types
  - Atomically executed in-order; no re-entrancy possible after this instruction

#### ELR_EL1 (Exception Link Register at EL1)
- **Address:** System register (MSR/MRS only)
- **Width:** 64-bit (full aarch64 VA space)
- **Content:** Return address (PC) where exception occurred, or resumable instruction if SP same
- **Semantics:** In data abort / instruction abort, points to the faulting instruction; software often adds 4 or 8 to resume
- **Used here:** Diagnostic dump to show where panic originated

#### ESR_EL1 (Exception Syndrome Register at EL1)
- **Address:** System register (MSR/MRS only)
- **Width:** 64-bit
- **Bitfields (relevant to kernel panic context):**
  - Bits [31:26]: Exception Class (EC) — encodes the exception type:
    - `0x00` = Unknown
    - `0x01` = Trapped WFx (WFI/WFE)
    - `0x02` = Trapped MCR/MRC (CP15)
    - `0x03` = Trapped MCRR/MRRC (CP15)
    - ... (many more defined in ARM DDI 0487)
    - `0x24` = Data abort (lower EL) — most common in user crashes
    - `0x25` = Data abort (same EL) — kernel page faults
    - `0x26` = Stack pointer alignment fault
    - `0x30` = SError interrupt (Async-abort)
    - etc.
  - Bits [24:0]: Exception-class-specific details (ISS, Instruction-specific syndrome)
- **Used here:** Diagnostic dump to identify exception type that triggered panic

#### FAR_EL1 (Fault Address Register at EL1)
- **Address:** System register (MSR/MRS only)
- **Width:** 64-bit VA
- **Content:** Virtual address that caused a page fault / access abort
- **Validity:** Only valid if ESR_EL1.EC indicates a synchronous data/instruction abort
- **Used here:** Diagnostic dump to show the bad address (for crashes like dereferencing invalid pointers)

#### SPSR_EL1 (Saved Processor State Register at EL1)
- **Address:** System register (MSR/MRS only)
- **Width:** 64-bit
- **Bitfields (AArch64 PSR layout):**
  - Bits [31:28]: N, Z, C, V (condition flags from user/kernel code)
  - Bits [27:24]: Q (saturation flag)
  - Bit 21: SS (software step debug flag)
  - Bit 20: IL (illegal execution state flag)
  - Bit 19: GE[3:0] (DSP flags, if NEON disabled)
  - Bits [9:6]: D, A, I, F (DAIF interrupt masks as saved)
  - Bits [5:4]: EL (exception level of interrupted code, or M bits in legacy)
  - Bits [3:0]: M (mode bits — for EL0 typically 0; for EL1 kernel entry, 0)
- **Used here:** Diagnostic dump to show CPU state when exception occurred

#### SP (Stack Pointer)
- **Alias:** SP_EL1 when read from EL1, SP_EL0 if in EL0 context (user task)
- **Width:** 64-bit VA
- **Operation used:** `mov %0, sp` (register-to-register move, not MSR/MRS)
- **Content:** Top (or head) of current task's kernel or user stack
- **Used here:** Diagnostic dump to aid in stack trace reconstruction

### UART Hardware Interface (used for console output)

Defined in `uart.h`:
```c
#define UART_BASE 0x09000000UL
#define UART_DR (UART_BASE + 0x00)
#define UART_FR (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR (UART_BASE + 0x30)
#define UART_ICR (UART_BASE + 0x44)
```

Functions called by panic:
- `uart_println(str)` — write string + newline
- `uart_printf(fmt, ...)` — formatted printf-style output

**Critical assumption:** UART is already initialized by the time panic can be called. Panic **cannot initialize UART**.

---

## Calling Context & Dependencies

### When `kernel_panic` is Called

From `kernel.c`:
1. **Boot path:** `kernel_panic_return()` function exists (fallback if `kernel_main()` unexpectedly returns)
2. **Runtime:** Any kernel code can invoke `kernel_panic(msg)` to signal an unrecoverable error
3. **Exception path:** Likely called from exception handlers when a fault cannot be recovered

### Subsystem Dependencies

**Hard dependencies:**
- **UART subsystem** (`lib/uart/uart.h`): `uart_println`, `uart_printf` must be working
  - Called from exception handler; UART must be pre-initialized
  - If UART fails, diagnostic output is lost but halt still occurs

**Soft dependencies:**
- No heap allocation
- No dynamic memory
- No locks (function must work even if lock system is broken)
- No interrupts after first instruction (all interrupts masked immediately)

### Invocation Points Observed in C Codebase

1. **`kernel.c` — `kernel_panic_return()` function:**
   ```c
   void kernel_panic_return(void) {
     kernel_panic("kernel_main returned unexpectedly");
   }
   ```
   - Shows example of calling with a string message

---

## Boot/Usage Ordering

1. **Early boot (before MMU, before exceptions):**
   - UART must be initialized first
   - Exception handlers must be installed
   - Panic can be called afterward

2. **Before first user task:**
   - Panic is callable but not expected to be triggered
   - If triggered, prints diagnostic info and halts

3. **After user tasks running:**
   - Exception handlers may call panic if a fault is unrecoverable
   - Panic halts the entire system
   - Example: kernel crashes, page fault in interrupt handler, etc.

**Critical sequencing note:** UART must be initialized **before** any code path that could call panic.

---

## Rust Porting Strategy

### Module Structure

```
src/lib/panic/
├── mod.rs              # Public API: pub fn kernel_panic(msg: &CStr) -> !
├── panic_asm.s         # Assembly: daifset, wfi instructions
└── [optional] tests.rs # Integration tests (if applicable)
```

### Key Types & Statics

**No static state required.** Panic is stateless—each invocation is independent.

### Rust API Design

```rust
/// Panic handler: capture state, print diagnostic, halt CPU
/// 
/// # Safety
/// 
/// Calls inline assembly to read system registers and mask interrupts.
/// Must only be called from EL1 (kernel context). Never returns.
#[noreturn]
pub fn kernel_panic(msg: Option<&CStr>) -> ! {
    // 1. Mask all interrupts (inline asm)
    mask_daif();
    
    // 2. Capture caller's LR
    let caller_lr = caller_return_address();
    
    // 3. Print banner via UART
    uart_println("");
    uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    uart_println("!!!         KERNEL PANIC            !!!");
    uart_println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    uart_println("");
    
    // 4. Print optional message
    if let Some(m) = msg {
        uart_printf(c"  Reason: %s\n", m.as_ptr());
    }
    
    // 5. Capture system registers (inline asm)
    let elr = read_elr_el1();
    let esr = read_esr_el1();
    let far = read_far_el1();
    let spsr = read_spsr_el1();
    let sp = read_sp();
    
    // 6. Print diagnostic dump
    uart_println("");
    uart_printf(c"  ELR_EL1  (return addr) : %x\n", elr);
    uart_printf(c"  ESR_EL1  (syndrome)    : %x\n", esr);
    uart_printf(c"  FAR_EL1  (fault addr)  : %x\n", far);
    uart_printf(c"  SPSR_EL1 (saved state) : %x\n", spsr);
    uart_printf(c"  SP       (stack ptr)   : %x\n", sp);
    uart_printf(c"  LR       (caller pc)   : %x\n", caller_lr);
    uart_printf(c"\n  System halted. Reset to continue.\n");
    
    // 7. Halt CPU (never returns)
    loop {
        halt_wfi();
    }
}
```

### Assembly Components (Remaining in .s or inline asm)

**Why assembly is necessary:**
- `msr daifset, #0xf` — system register write for interrupt masking
- `mrs` instructions — reading ELR_EL1, ESR_EL1, FAR_EL1, SPSR_EL1 (system registers)
- `wfi` — CPU halt instruction
- `__builtin_return_address(0)` — compiler-specific intrinsic for LR capture

**Option A: Inline asm in Rust (preferred for simplicity)**
```rust
#[inline(always)]
unsafe fn mask_daif() {
    asm!("msr daifset, #0xf", options(preserves_flags, nostack));
}

#[inline(never)]
unsafe fn read_elr_el1() -> u64 {
    let val: u64;
    asm!("mrs {}, elr_el1", out(reg) val, options(preserves_flags, nostack));
    val
}
// ... similar for other sysregs

#[inline(always)]
unsafe fn halt_wfi() -> ! {
    asm!("wfi", options(nostack, noreturn));
}
```

**Option B: External .s file (if inline asm becomes unwieldy)**
```asm
; panic_asm.s — functions for system register access
.global panic_mask_daif
panic_mask_daif:
    msr daifset, #0xf
    ret

.global panic_read_elr_el1
panic_read_elr_el1:
    mrs x0, elr_el1
    ret

; etc.

.global panic_halt_wfi
panic_halt_wfi:
    wfi
    b panic_halt_wfi  ; infinite loop
```

### Locking & Static State

- **No locks needed:** panic is called once and never returns
- **No static state:** no initialization required
- **Exception-safe:** works even if lock system is broken

### FFI to UART Subsystem

Panic calls `uart_println` and `uart_printf` which are already in the UART Rust module:
```rust
extern "C" {
    pub fn uart_println(s: *const c_char);
    pub fn uart_printf(fmt: *const c_char, ...) -> c_int;
}
```

Or use Rust bindings from the uart module if available.

### Error Handling

- No error handling: panic is the handler of last resort
- If UART is broken, diagnostic output is silent but halt still occurs
- If UART panic print fails, loop is still entered

### Testing Strategy

1. **Unit tests:** Can't easily test a `noreturn` function, but can test register-reading helper functions
2. **Integration test:** Boot kernel, intentionally trigger a panic, verify:
   - UART output appears on console
   - System halts (no further kernel output)
   - Reset required to recover

---

## Gotchas & Correctness Requirements

### Critical Implementation Details

1. **Interrupt masking happens FIRST:**
   - The inline `msr daifset, #0xf` must be the first instruction in the C function body
   - Any prior call or register access could be interrupted
   - **Wrong:** Call a function, then mask interrupts → re-entrance possible
   - **Right:** Mask interrupts immediately → no re-entrance

2. **`__builtin_return_address(0)` must be called before any other function call:**
   - Every function call (including `uart_println`) can modify x30 (LR)
   - **Wrong:** Call `uart_println`, then read `__builtin_return_address(0)` → gets wrong address
   - **Right:** Read `__builtin_return_address(0)` into a local variable first, then call uart

3. **`volatile` on all inline asm blocks:**
   - `asm! volatile` prevents the compiler from speculating or reordering register reads
   - Without `volatile`, the compiler might optimize away reads or reorder them after UART calls
   - Each register read must include `options(volatile)` or equivalent

4. **`"memory"` clobber on DAIF write:**
   - `msr daifset` affects CPU behavior globally; compiler must not assume memory access ordering
   - Clobber `"memory"` to prevent instruction reordering across this instruction

5. **System registers read-only:**
   - All MRS instructions are safe (no side effects on read)
   - No need for special barriers before/after reads

6. **SP might be user or kernel:**
   - If panic is called from user-space exception handler, SP is SP_EL0 (user stack)
   - If panic is called from kernel code, SP is SP_EL1 (kernel stack)
   - Value printed is whatever is current; no need to check EL

7. **FAR_EL1 only valid after data/instruction abort:**
   - If panic is called for a non-fault reason (e.g., explicit `kernel_panic("out of memory")`), FAR_EL1 contains stale data
   - This is acceptable—diagnostic dump is "best effort" (useful if we got here from a fault)

8. **Caller's LR is x30 saved-in-link-register on entry to `kernel_panic`:**
   - The exact value depends on how `kernel_panic` was called
   - If tail-called (via `jmp` in asm), LR points to the caller of the code that called panic
   - If called normally (via `bl` or `blr`), LR points to the instruction after the `bl` instruction
   - The `__builtin_return_address(0)` uses AAPCS64-defined unwinding to get this right

9. **Never add C function calls after DAIF mask if possible:**
   - Each call risks clobbering x30, x0-x7, and FPSR
   - However, we *must* call UART, so ensure those calls don't re-enter panic
   - UART is interrupt-safe and non-reentrant (with DAIF masked)

10. **Loop with `wfi` is truly infinite:**
    - `while (1) { asm volatile("wfi"); }` is correct
    - The CPU will halt, and no interrupt will wake it (DAIF [3:0] = 1111)
    - External reset or PSCI from another CPU is the only way out

---

## Summary for Implementer

**The panic subsystem is small and critical:**
- ~50 lines of C code in the original
- Minimal dependencies (UART only)
- Must be rock-solid (called when everything else failed)

**Key invariants to preserve:**
1. Interrupt masking first
2. LR capture before any function call
3. `volatile` on all asm register reads
4. `"memory"` clobber on interrupt mask write
5. Infinite loop at end with `wfi` — CPU halts permanently

**Rust translation should be mechanical:** replace C inline asm with Rust inline asm, use wrapper functions for register reads, preserve call ordering.

