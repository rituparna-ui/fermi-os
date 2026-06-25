# Fermi OS Utils Subsystem - C Reference & Rust Porting Spec

## Overview

The utils subsystem is a lightweight collection of miscellaneous kernel helper functions and macros used during early boot and throughout the kernel's lifetime. It provides:

1. **Exception Level Detection & Display** (`print_current_el`): Reads the ARM64 CurrentEL system register to determine the current execution privilege level (EL0/EL1/EL2/EL3) and prints a human-readable description.
2. **Data Synchronization Barrier** (`dsb_sy`): Issues a full system data synchronization barrier (DSB SY) to enforce strict memory ordering when required.
3. **Status Constants**: Simple return codes (ESUCCESS=1, EERROR=0) used by other kernel subsystems.

## Public API

### Constants

```c
#define ESUCCESS 1  // Success status code
#define EERROR   0  // Error status code
```

These constants are used as return values throughout the kernel (e.g., `fat32_mount()` returns `ESUCCESS`).

### Functions

#### `void print_current_el(void)`

**Signature**: No parameters, no return value.

**Behavior**:
1. Reads the ARM64 `CurrentEL` system register using inline assembly (`mrs %0, CurrentEL`)
2. Extracts bits [3:2] which encode the current exception level (0–3)
3. Maps the EL number to a human-readable string:
   - 0 → "User Space"
   - 1 → "Kernel Space"
   - 2 → "Hyper Space"
   - 3 → "Secure Monitor/Firmware"
   - Other → "Invalid Exception Level"
4. Calls `uart_printf("Current Exception Level: %s\n", el_name)` to output the result

**Used by**:
- `early_init()` in kernel.c during boot (immediately after `uart_init()` and `uart_println("Fermi OS - Booting Up...")`)
- Confirms the kernel booted at the expected privilege level

**Hardware Details**:
- ARM64 System Register: `CurrentEL`
- Read-only; encodes current exception level in bits [3:2]
- All other bits are reserved (zero on ARMv8)

#### `void dsb_sy(void)`

**Signature**: No parameters, no return value.

**Behavior**:
1. Issues a Data Synchronization Barrier with a "Sync" domain scope using inline assembly: `dsb sy`
2. Clobbers the "memory" constraint to ensure the compiler doesn't reorder memory operations across this barrier
3. Returns immediately after the barrier completes

**Semantics**:
- DSB (Data Synchronization Barrier) waits for all explicit memory operations before it to complete
- The "sy" (Sync) variant applies to all observers in the same Inner Shareable domain
- Common use: enforcing order before MMU configuration, exception handler setup, or cache maintenance

**Gotchas**:
- Does NOT provide instruction ordering (use ISB for that)
- Does NOT provide cache coherence across clusters in a non-shareable domain
- Can be expensive; only use when strict ordering is required

---

## Architecture-Specific Details

### ARM64 CurrentEL Register

**Location in System Register File**: `S3_0_C4_C2_0`

**Bit Layout**:
```
[63:4]  - Reserved, SBZ (should be zero)
[3:2]   - EL (Exception Level) — the bits we extract
[1:0]   - Reserved, SBZ
```

**Exception Level Encoding**:
```
EL[3:2]  | Meaning
---------|-------------------
00       | EL0 (User Space)
01       | EL1 (Kernel Space)
10       | EL2 (Hyper Space)
11       | EL3 (Secure Monitor/Firmware)
```

### DSB Instruction

**Encoding**: `dsb #15` (immediate 15 = SY domain)

**Domains**:
- `#15` / `sy`: Sync (all observers, inner shareable)
- `#14` / `ish`: Inner Shareable
- `#13` / `nsh`: Non-Shareable
- `#12` / `osh`: Outer Shareable

---

## Dependency Graph

### Dependencies (what utils calls)

- **uart/uart.h**: `uart_printf()` — used by `print_current_el()` to output the exception level string
- **No other kernel subsystems** — utils is a foundational helper with minimal dependencies

### Depended On By

- **kernel.c**: Calls `print_current_el()` during `early_init()`
- **Other subsystems**: May use `ESUCCESS` / `EERROR` constants or `dsb_sy()` if they need strong memory barriers

---

## Boot/Usage Ordering

### Early Boot Sequence (relevant to utils)

1. **early_init()** (before MMU, in PAS — physical address space):
   - `zero_bss()` — zero BSS section
   - `enable_fp_simd()` — enable SIMD/FP registers for varargs
   - `uart_init()` — initialize UART
   - `uart_println("Fermi OS - Booting Up...")`
   - **`print_current_el()`** ← *utils call here* — confirms boot EL
   - `exceptions_init()` — set up exception handlers
   - ... rest of boot ...

2. **kernel_main()** (after MMU, in VAS — virtual address space):
   - Other initialization (no direct utils calls in kernel_main)

### Usage Pattern for DSB

`dsb_sy()` is called by other subsystems (not shown in the provided files) when:
- Configuring page tables and needing to flush TLBs
- Setting up exception handlers and updating VBAR_EL1
- Updating memory attributes in cache maintenance

---

## Exact Return Codes

The utils subsystem defines two simple return codes used throughout the kernel:

```c
ESUCCESS  1   // Operation succeeded; truthy in C
EERROR    0   // Operation failed; falsy in C
```

**Example Usage** (from kernel.c):
```c
if (fat32_mount() != ESUCCESS) {
    uart_printf("[FS][FAT32] Unable to mount file system");
}
```

---

## Rust Porting Strategy

### Module Structure

```
kernel::utils
├── pub fn print_current_el()      // Query CurrentEL and print via UART
├── pub fn dsb_sy()                // Memory barrier instruction
├── const ESUCCESS: i32 = 1
└── const EERROR: i32 = 0
```

### Types & Statics

No global mutable state is needed. The module is purely functional:

- **`print_current_el()`**: Pure query + side effect (UART write); no state
- **`dsb_sy()`**: Pure instruction side effect; no state
- Constants are compile-time values

### Ownership & Locking

No locking required; both functions are:
- Read-only (CurrentEL is read-only system register)
- Side-effect free (except UART output, which is already handled by uart module)
- No shared mutable state

### Assembly Strategy

Both functions **MUST** remain inline assembly (cannot be ported to pure Rust):

1. **`print_current_el()`**:
   - Read CurrentEL system register → requires `mrs` instruction
   - Pure Rust has no way to read system registers
   - Must use `inline_asm!` or `asm!` macro

2. **`dsb_sy()`**:
   - DSB instruction has no Rust equivalent
   - Must use `inline_asm!` macro with memory clobber

### Implementation Outline

```rust
// src/lib/utils.rs

pub const ESUCCESS: i32 = 1;
pub const EERROR: i32 = 0;

/// Get the current exception level (0–3) by reading the CurrentEL system register.
/// Bits [3:2] encode: 0=EL0, 1=EL1, 2=EL2, 3=EL3
fn get_current_el_number() -> u8 {
    let el: u64;
    unsafe {
        core::arch::asm!(
            "mrs {}, CurrentEL",
            out(reg) el,
            options(nostack, preserves_flags, readonly)
        );
    }
    ((el >> 2) & 0b11) as u8
}

/// Map exception level (0–3) to human-readable name
fn get_el_name(el: u8) -> &'static str {
    match el {
        0 => "User Space",
        1 => "Kernel Space",
        2 => "Hyper Space",
        3 => "Secure Monitor/Firmware",
        _ => "Invalid Exception Level",
    }
}

/// Print the current exception level via UART
pub fn print_current_el() {
    let el = get_current_el_number();
    let el_name = get_el_name(el);
    crate::uart::uart_printf("Current Exception Level: %s\n", el_name);
}

/// Issue a full system data synchronization barrier (DSB SY)
/// Ensures all prior memory operations complete before returning
pub fn dsb_sy() {
    unsafe {
        core::arch::asm!(
            "dsb sy",
            options(nostack, preserves_flags, fence(aqrel))
        );
    }
}
```

### Assembly Rationale

| Function | Asm Required | Why |
|----------|--------------|-----|
| `print_current_el()` | YES | Read system register (CurrentEL) — only possible via `mrs` |
| `dsb_sy()` | YES | Data Synchronization Barrier — no Rust built-in |
| `get_current_el_number()` | YES | Part of reading CurrentEL |
| `get_el_name()` | NO | Pure Rust pattern matching |

### Inline Asm Options Rationale

**For `mrs` (read CurrentEL)**:
- `nostack`: Doesn't use stack
- `preserves_flags`: Doesn't modify NZCV flags
- `readonly`: Read-only operation; optimizer can optimize away redundant reads if safe

**For `dsb sy`**:
- `nostack`: Doesn't use stack
- `preserves_flags`: Doesn't modify NZCV flags
- `fence(aqrel)`: Emits a memory fence; the Rust compiler will respect memory dependencies across this barrier

---

## Gotchas & Subtle Issues

1. **CurrentEL is Read-Only**:
   - Cannot be written; attempting to set it via `msr` raises an exception
   - Only valid to read via `mrs`

2. **EL Encoding is Fixed**:
   - Bits [3:2] always contain the EL number
   - All other bits are reserved/zero
   - Mask with `0b11` after shifting to be safe

3. **DSB SY vs Other Barriers**:
   - `dsb sy` is expensive; use ISB (Instruction Synchronization Barrier) if only instruction ordering is needed
   - DSB SY waits for *all* memory operations; if only flushing a specific cache line is needed, consider `dc` instructions instead

4. **Memory Ordering Constraints**:
   - DSB SY ensures ordering relative to inner shareable observers (same cluster)
   - For cross-cluster coherence on big.LITTLE systems, may need additional IPI or sev/sevl

5. **UART Dependency**:
   - `print_current_el()` assumes UART is initialized
   - If called before `uart_init()`, will panic or produce garbage
   - Should only be called after `uart_init()` in early_init()

6. **Compiler Optimization**:
   - The `mrs` instruction is cheap (typically 2 cycles); modern CPUs pipeline it well
   - Repeated calls to `print_current_el()` are rare in practice
   - No memoization needed; the EL doesn't change at runtime

---

## Constants Summary

| Name | Value | Usage |
|------|-------|-------|
| `ESUCCESS` | 1 | Fat32 mount, other subsystem returns |
| `EERROR` | 0 | Falsy error code |
| `UART_BASE` | 0x09000000UL | UART MMIO base (from uart.h; not directly used by utils) |
| ARM64 CurrentEL bits | [3:2] | EL encoding; mask `(reg >> 2) & 0b11` |

---

## Integration Notes

### With UART

- `print_current_el()` uses `uart_printf(const char *fmt, ...)` with a format string
- This requires UART to be initialized first
- No buffering; output goes directly to UART TX

### With Other Subsystems

- `ESUCCESS` / `EERROR` are simple return codes; no interop complexity
- `dsb_sy()` is a general-purpose memory barrier; no subsystem specifics

---

## Summary for Implementer

**Key Tasks**:

1. Define `ESUCCESS = 1` and `EERROR = 0` constants (may live in a separate error codes module)
2. Implement `get_current_el_number()` using inline `mrs CurrentEL` assembly
3. Implement `get_el_name()` as a simple `match` on the EL number
4. Implement `print_current_el()` that calls both and formats output via UART
5. Implement `dsb_sy()` as a standalone `dsb sy` instruction with memory fence semantics
6. Test on actual hardware: confirm `print_current_el()` prints "Kernel Space" when called from `early_init()` (EL1 entry point)

**File Locations** (proposed):
- `/src/utils/mod.rs` — main module with all four functions
- No additional files needed; this is a minimal subsystem
