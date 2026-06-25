# GICv3 Controller Porting Specification

## Overview

The GIC (Generic Interrupt Controller) subsystem implements GICv3 affinity-routing-mode interrupt handling for the Fermi aarch64 bare-metal kernel. It manages:

1. **Distributor initialization** (GICD): enables non-secure Group 1 interrupts and affinity routing mode
2. **Redistributor activation** (GICR): wakes CPUs from sleep, configures per-CPU interrupt groups
3. **System register interface**: enables sysreg-based acknowledgement/EOI instead of MMIO ICC interface
4. **Interrupt enabling**: routes SPIs (Shared Peripheral Interrupts) through distributor, SGI/PPI through redistributor
5. **IRQ dispatch**: acknowledgement, counting, and end-of-interrupt handling via sysregs (icc_iar1_el1, icc_eoir1_el1)
6. **/proc/interrupts support**: per-INTID counters and formatted interrupt table rendering

The implementation is **single-core** (boot CPU only) in the C version but architecturally suitable for SMP expansion.

## Hardware Layout

### Memory Mapped Regions

```
GICD_BASE = 0x08000000  (Distributor base)
GICR_BASE = 0x080A0000  (Redistributor base for CPU 0)
```

### GICD Registers (Distributor)

| Offset | Name | Field | Value/Bits | Purpose |
|--------|------|-------|------------|---------|
| 0x0000 | GICD_CTLR | - | RW | Control register |
| | | ENABLE_G1NS | [1] | Enable Group 1 Non-Secure (1=enabled) |
| | | ARE_NS | [4] | Affinity Routing Enable Non-Secure (1=enabled) |
| 0x0100 | GICD_ISENABLER[n] | [31:0] | RW | Interrupt Set-Enable (1 bit per INTID; n = INTID/32) |

**GICD_ISENABLER Array**: ISENABLERn at offset 0x0100 + (n*4), where n = INTID / 32
- Applies to SPIs (INTID >= 32)
- Each bit enables one INTID within the 32-bit word
- Bit position within word = INTID % 32

### GICR Registers (Redistributor for one CPU)

| Offset | Name | Field | Value/Bits | Purpose |
|--------|------|-------|------------|---------|
| 0x0014 | GICR_WAKER | - | RW | Redistributor wakeup control |
| | | PROCESSOR_SLEEP | [1] | 1=processor asleep, 0=awake |
| | | CHILDREN_ASLEEP | [2] | 1=children asleep (RO), 0=children awake |
| 0x10000 | GICR_SGI_BASE | - | - | Base offset for SGI/PPI registers |
| 0x10000 + 0x0080 | GICR_IGROUPR0 | [31:0] | RW | SGI/PPI Interrupt Group (0=G0, 1=G1) |
| 0x10000 + 0x0D00 | GICR_IGRPMODR0 | [31:0] | RW | SGI/PPI Interrupt Group Modifier (0=Secure, 1=NS) |
| 0x10000 + 0x0100 | GICR_ISENABLER0 | [31:0] | RW | SGI/PPI Set-Enable (1 bit per INTID 0-31) |

### System Registers (aarch64)

| Register | Op0 | Op1 | CRn | CRm | Op2 | Purpose |
|----------|-----|-----|-----|-----|-----|---------|
| ICC_SRE_EL1 | 3 | 0 | 12 | 12 | 5 | System Register Enable (sysreg access vs MMIO) |
| ICC_PMR_EL1 | 3 | 0 | 4 | 6 | 0 | Priority Mask Register |
| ICC_IGRPEN1_EL1 | 3 | 0 | 12 | 12 | 7 | Interrupt Group 1 Enable |
| ICC_IAR1_EL1 | 3 | 0 | 12 | 8 | 0 | Interrupt Acknowledge Register (read) |
| ICC_EOIR1_EL1 | 3 | 0 | 12 | 8 | 1 | End of Interrupt Register (write) |

**DAIFCLR register**: system-level IRQ mask
- Bit [1] when written unmasks physical IRQs (clears I bit in PSTATE)

## Interrupt ID Layout

```
0-15     SGI (Software Generated Interrupts)
16-31    PPI (Private Peripheral Interrupts) + SGI
32-1019  SPI (Shared Peripheral Interrupts)
1023     No pending interrupt (special pseudovalue)
```

**Important values**:
- `TIMER_PPI_INTID = 30` (ARM generic timer interrupt, routed via redistributor)
- `GIC_INTID_NO_PENDING = 1023` (returned by ICC_IAR1_EL1 when no interrupt pending; should not call EOI)

## Public API

### Initialization

```c
void gic_init(void)
```

**Behavior**:
1. Enable system register interface via `mrs/msr icc_sre_el1` (bit [0] = 1)
2. Enable distributor affinity routing: write `GICD_CTLR = 0x12` (ARE_NS | ENABLE_G1NS)
3. Wake redistributor: clear `GICR_WAKER[PROCESSOR_SLEEP]` and poll until `GICR_WAKER[CHILDREN_ASLEEP]` clears
4. Configure SGI/PPI groups: write `GICR_IGROUPR0 = 0xFFFFFFFF` (all Group 1), `GICR_IGRPMODR0 = 0x00000000` (all non-secure)
5. Set priority mask to accept all: `msr icc_pmr_el1, #0xFF`
6. Enable Group 1 interrupts: `msr icc_igrpen1_el1, #0x1`
7. Issue ISB to ensure system register writes complete
8. Unmask physical IRQs at CPU level: `msr daifclr, #2` (clears I bit in PSTATE)

**Call sequence**: Must be called once during kernel boot, before any IRQ can be delivered.

**Side effects**: 
- Enables IRQ delivery globally
- All UART debug output during init indicates success

### IRQ Enable

```c
void gic_enable_irq(uint32_t intid)
```

**Behavior**:
- **SGI/PPI (intid < 32)**: Set bit `intid % 32` in `GICR_ISENABLER0` (redistributor)
- **SPI (intid >= 32)**: Set bit `intid % 32` in `GICD_ISENABLER[(intid / 32) * 4]` (distributor)

**Return**: void (logging only)

### IRQ Acknowledgement

```c
uint64_t gic_ack_irq(void)
```

**Inline assembly**: 
```asm
mrs %0, icc_iar1_el1
```

**Returns**: 64-bit INTID value
- **Valid INTID**: 0-1019 (actual pending interrupt)
- **Special pseudovalue 1023**: No interrupt pending; **must not call gic_end_irq()**

**Semantics**: Atomically acknowledges the highest-priority pending interrupt and marks it in-service. Called once per IRQ exception in the exception handler.

### IRQ Counting

```c
void gic_count_irq(uint32_t intid)
```

**Behavior**: Increment the per-INTID counter for `intid` (if `intid < GIC_COUNTERS_MAX`).

**Storage**:
- Static array: `static uint64_t irq_counts[256]` (1 KiB)
- Covers INTID 0-255; INTID >= 256 silently ignored (no /proc impact)

**Called**: In exception handler immediately after gic_ack_irq(), before any ISR-specific handling.

**Rationale**: Excludes spurious/pending pseudointid (1023) since gic_ack_irq() returns 1023 only when no actual interrupt pending.

### IRQ End-of-Interrupt

```c
void gic_end_irq(uint64_t intid)
```

**Inline assembly**:
```asm
msr icc_eoir1_el1, %0
```

**Argument**: The INTID value from gic_ack_irq() (must be 0-1019, **not 1023**).

**Semantics**: Marks interrupt as handled and allows GIC to deliver future interrupts of this or lower priority.

**Called**: After ISR-specific handling in exception dispatch, before scheduling/context switch.

### /proc/interrupts Rendering

```c
void gic_count_irq(uint32_t intid)
```

Helper to classify interrupt source:

```c
static const char *gic_intid_source(uint32_t intid)
```

Returns static string based on INTID:
- `intid == 30`: "timer (PPI)"
- `intid < 16`: "SGI"
- `intid < 32`: "PPI"
- `intid >= 32`: "SPI"

### Interrupt Table Rendering

```c
int gic_render_interrupts(char *buf, uint32_t buflen)
```

**Behavior**:
1. Write header line: `"INTID  COUNT     SOURCE\n"`
2. For each INTID 0-255 with non-zero count:
   - Format: `"%u    %u  %s\n"` (intid, count, source_string)
   - Uses `ksnprintf()` into temporary 64-byte buffer
   - Append to output, stopping when buffer full
3. Return total bytes written (including NUL terminator offset)

**Return value**: (int) bytes written to buf

**Buffer safety**: Carefully tracks available space; will not overflow buf.

## Initialization Sequence

1. **Early boot (EL1)**:
   - UART already initialized (used for debug output)
   - Call `gic_init()` from `kernel_main()` before other PCI/device initialization
   - Called after `cpu_init()` but before `pci_enumerate_bus()`

2. **After `gic_init()` completes**:
   - IRQs are fully enabled and live
   - Timer can be armed (will fire at PPI intid=30)
   - Other devices can enable via `gic_enable_irq(intid)`

3. **Exception dispatch integration**:
   - Vector table routes IRQs to `exception_dispatch(EXCEPTION_IRQ, frame)`
   - Dispatch code calls: `gic_ack_irq()` → `gic_count_irq()` → device ISR → `gic_end_irq()` → `schedule()`
   - Special case: `intid == 1023` (no interrupt pending) → break early without EOI

## Call Flow in Exception Handler

```c
void exception_dispatch(uint64_t type, trap_frame_t *frame) {
  ...
  case EXCEPTION_IRQ: {
    uint32_t intid = gic_ack_irq();
    if (intid == GIC_INTID_NO_PENDING) {
      break;  // spurious; no EOI
    }
    gic_count_irq(intid);
    
    if (intid == TIMER_PPI_INTID) {
      timer_handle_irq();
    } else {
      uart_printf("[IRQ] INTID %d (not implemented)\n", (uint64_t)intid);
    }
    
    gic_end_irq(intid);  // EOI AFTER handler
    schedule();
    break;
  }
  ...
}
```

## Rust Port Strategy

### Module Structure

```
kernel::gic
├── init.rs         // gic_init(), wakeup sequences
├── regs.rs         // MMIO/sysreg constants, bitfields
├── irq_enable.rs   // gic_enable_irq() implementation
├── dispatch.rs     // ack/eoi, system register ops
└── stats.rs        // irq_counts[], gic_render_interrupts()
```

### Core Types

```rust
/// Interrupt ID type; 0-1019 valid, 1023 = no pending
pub type IntId = u32;

/// Acknowledging an interrupt; returned by gic_ack_irq()
pub type AckToken = u64;  // wraps raw ICC_IAR1_EL1 result

/// GIC controller state (mostly static HW; minimal runtime state)
pub struct GicController {
    // No mutable state needed during normal operation;
    // all register I/O is intrinsic (reads/writes are volatile)
    // irq_counts is a separate static
}

/// Per-interrupt counter array
static mut IRQ_COUNTS: [u64; 256] = [0; 256];
```

### Key Design Decisions

1. **No dynamic allocation**: All GIC state is static (register MMIO, fixed-size counter array)
2. **No locking needed** (single-core initial port): IRQ counting increments are atomic (uint64 write on aarch64); later SMP expansion may need atomics
3. **Register access**:
   - MMIO reads/writes via volatile pointers (core::ptr::read_volatile, write_volatile)
   - System registers via inline asm (mrs/msr) with ISB barriers
4. **Affinity routing mode** only (no legacy distributor mode); simplifies design
5. **No nested masking**: priority mask fixed at 0xFF (accept all); simplifies design for single-core

### Assembly Requirements

The following **must remain inline assembly** (cannot be expressed as volatile MMIO):

1. **System register enable** (gic_init):
   ```asm
   mrs %0, icc_sre_el1
   [modify]
   msr icc_sre_el1, %0
   isb
   ```

2. **IRQ acknowledgement** (gic_ack_irq):
   ```asm
   mrs %0, icc_iar1_el1
   ```

3. **Priority mask** (gic_init):
   ```asm
   msr icc_pmr_el1, %0  // %0 = 0xFF
   ```

4. **Group 1 enable** (gic_init):
   ```asm
   msr icc_igrpen1_el1, %0  // %0 = 0x1
   isb
   ```

5. **End-of-interrupt** (gic_end_irq):
   ```asm
   msr icc_eoir1_el1, %0  // %0 = intid
   ```

6. **IRQ unmask at CPU** (gic_init):
   ```asm
   msr daifclr, #2  // clear I bit
   ```

### Locking & Synchronization

**Single-core port** (current):
- IRQ counting: no lock needed; each CPU updates its own counter range conceptually (but only one CPU here)
- Interrupt enable: called from init or from scheduler; no preemption until IRQs running; safe

**SMP expansion notes**:
- IRQ counts: may want atomic increments (or per-CPU counters, read-sum on /proc)
- Interrupt enable: distributor/redistributor writes may race; spinlock advised
- ESR/exception state: per-CPU already (sysregs)

### Public Rust API Surface

```rust
/// Initialize GICv3 (affinity routing mode, sysreg interface).
/// Called once during boot before IRQs delivered.
pub fn init();

/// Enable interrupt delivery for intid (SGI/PPI via redistributor, SPI via distributor).
pub fn enable_irq(intid: u32);

/// Acknowledge pending interrupt (reads ICC_IAR1_EL1).
/// Returns intid (0-1019) or 1023 if no pending.
/// Do not call gic_end_irq() if result is 1023.
pub fn ack_irq() -> u32;

/// Count this interrupt in statistics.
/// Called in exception dispatch after ack.
pub fn count_irq(intid: u32);

/// End-of-interrupt (writes ICC_EOIR1_EL1).
/// Must pass valid intid (not 1023).
pub fn end_irq(intid: u64);

/// Render /proc-style interrupt table into buffer.
/// Returns bytes written.
pub fn render_interrupts(buf: &mut [u8]) -> usize;
```

### Constants in Rust

```rust
pub const GICD_BASE: u64 = 0x08000000;
pub const GICR_BASE: u64 = 0x080A0000;

pub const GICD_CTLR: u64 = GICD_BASE + 0x0000;
pub const GICD_ISENABLER: u64 = GICD_BASE + 0x0100;

pub const GICD_CTLR_ENABLE_G1NS: u32 = 1 << 1;
pub const GICD_CTLR_ARE_NS: u32 = 1 << 4;

pub const GICR_WAKER: u64 = GICR_BASE + 0x0014;
pub const GICR_SGI_BASE: u64 = GICR_BASE + 0x10000;
pub const GICR_IGROUPR0: u64 = GICR_SGI_BASE + 0x0080;
pub const GICR_IGRPMODR0: u64 = GICR_SGI_BASE + 0x0D00;
pub const GICR_ISENABLER0: u64 = GICR_SGI_BASE + 0x0100;

pub const GICR_WAKER_PROCESSOR_SLEEP: u32 = 1 << 1;
pub const GICR_WAKER_CHILDREN_ASLEEP: u32 = 1 << 2;

pub const GIC_INTID_NO_PENDING: u32 = 1023;
pub const GIC_COUNTERS_MAX: usize = 256;
pub const TIMER_PPI_INTID: u32 = 30;
```

## Gotchas & Portability Notes

### Critical Ordering & Barriers

1. **After SRE write**: Always issue `isb` (instruction synchronization barrier) to ensure register writes complete before dependent operations
2. **Redistributor wakeup**: Must poll `GICR_WAKER[CHILDREN_ASLEEP]` in tight loop until it clears; do not proceed without this
3. **EOI before schedule**: Must call `gic_end_irq()` before allowing other interrupts or context switch; if skipped, subsequent IRQs may not be delivered correctly
4. **No EOI on 1023**: Calling `gic_end_irq(1023)` is undefined; exception handler must explicitly check

### GICD_ISENABLER Array Indexing

When enabling SPI (intid >= 32):
- **Register address**: `GICD_ISENABLER + ((intid / 32) * 4)`
- **Bit within register**: `intid % 32`

Example: enable SPI 33
- Word index: 33 / 32 = 1
- Register: `0x08000100 + 0x4 = 0x08000104`
- Bit: 33 % 32 = 1
- Actual write: set bit [1] of register at 0x08000104

### Volatile I/O

All MMIO reads/writes must be volatile (not optimized away). In Rust:
```rust
ptr::read_volatile(addr as *const u32)
ptr::write_volatile(addr as *mut u32, val)
```

### System Register Constraints

- `icc_sre_el1[0]`: must be 1 to enable sysreg interface (else ICC_* registers inaccessible)
- `icc_pmr_el1`: all 1s (0xFF) = accept all priorities; 0 = mask all (not used here)
- `icc_igrpen1_el1[0]`: must be 1 to enable Group 1 IRQs (or no interrupts delivered)
- `daifclr[2]`: write any value; clears I bit (unmasks IRQs) at CPU level

### Single-Core Assumption

The C code initializes only one redistributor (`GICR_BASE = 0x080A0000`). For SMP:
- Each CPU has its own redistributor at `GICR_BASE + (cpuid * 0x20000)`
- Each needs separate wakeup and SGI/PPI config
- Timer/scheduler integration must account for per-CPU state

## Testing Hooks

1. **Timer IRQ validation**: Enable `gic_enable_irq(TIMER_PPI_INTID)`, set up timer, verify exception dispatch calls timer_handle_irq()
2. **Interrupt counting**: Add test device that fires IRQ, verify gic_count_irq() increments; read via /proc/interrupts
3. **Spurious IRQ handling**: Verify intid=1023 case (may occur under contention or misconfiguration)

## Files to Reference

- C Source: `src/exception/gic/gic.c`, `src/exception/gic/gic.h`
- Exception dispatch: `src/exception/exception.c` (case EXCEPTION_IRQ)
- Timer integration: `src/exception/timer/timer.h` (TIMER_PPI_INTID = 30)
- MMIO helpers: assumed in `src/mmio/mmio.h` (not in tree; infer MMIO semantics from usage)

---

**Spec Version**: 1.0  
**Target**: Fermi aarch64 bare-metal kernel, GICv3 affinity-routing mode (single-core initial)  
**Last Updated**: 2026-06-25
