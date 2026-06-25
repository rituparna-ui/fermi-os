# Timer Subsystem Specification (ARM Generic Timer)

## Overview

The timer subsystem drives periodic kernel ticks on AArch64 Fermi OS using the ARM Generic Timer (a mandatory feature of the ARMv8-A ISA). It operates via:

1. **Physical Timer (EL1)**: Uses the physical counter CNTPCT_EL0 and comparator CNTP_CVAL_EL0 (not virtual, to avoid divergence under virtualization)
2. **Periodic Interrupt**: Fires a PPI (Private Peripheral Interrupt) #30 every 10 ms, delivered by the GICv3
3. **Absolute Deadlines**: Uses CVAL (compare value) instead of TVAL (countdown) to avoid IRQ-latency jitter accumulation
4. **Tight Scheduler Integration**: Each timer tick wakes sleeping tasks and triggers the scheduler

The subsystem maintains four volatile statics accessed from both task and IRQ contexts:
- **timer_freq**: CNTFRQ_EL0 (e.g., 50 MHz physical timer frequency)
- **timer_interval**: Number of timer ticks per TIMER_INTERVAL_MS (10 ms default)
- **tick_count**: Total ticks since boot (monotonic)
- **tick_callback**: Optional user callback, fired on every tick

## Constants

```c
#define TIMER_PPI_INTID 30              // Private Peripheral Interrupt ID for physical timer
#define TIMER_INTERVAL_MS 10            // Tick interval in milliseconds (must not change at runtime)
```

## Public API

### timer_init()
```c
void timer_init(void)
```

**Behavior:**
- Reads CNTFRQ_EL0 (physical timer frequency in Hz, set by firmware)
- Calls `gic_enable_irq(TIMER_PPI_INTID)` to enable the PPI in GICv3
- Logs frequency to UART (e.g., "Frequency: 50000000 Hz (50 MHz)")
- Sets `tick_count = 0`
- **Must be called before timer_start()**
- **Single-threaded (early boot), not called from IRQ context**

### timer_start(uint64_t interval_ms)
```c
void timer_start(uint64_t interval_ms)
```

**Behavior:**
- Converts `interval_ms` to timer ticks: `timer_interval = timer_freq * interval_ms / 1000`
- Resets `tick_count = 0`
- Reads current physical timer count: `now = CNTPCT_EL0` (MRS instruction)
- Sets the absolute deadline: `CNTP_CVAL_EL0 = now + timer_interval`
- Enables the physical timer: `CNTP_CTL_EL0 = 1` (bit 0: ENABLE)
- **Will panic if called before timer_init()**
- **Single-threaded (early boot initialization)**
- **Note**: Uses absolute deadline (CVAL), NOT countdown (TVAL), so each tick drifts at most one interval even under high IRQ latency

### timer_stop()
```c
void timer_stop(void)
```

**Behavior:**
- Disables the physical timer: `CNTP_CTL_EL0 = 0`
- Logs total tick count to UART
- **Safe to call from any context (but normally only at shutdown)**

### timer_handle_irq()
```c
void timer_handle_irq(void)
```

**Behavior:**
- Increments `tick_count++`
- Re-arms the next deadline by reading current CVAL and adding one interval:
  - `cval = CNTP_CVAL_EL0` (MRS)
  - `cval += timer_interval`
  - `CNTP_CVAL_EL0 = cval` (MSR)
- **Absolutely critical**: The re-arming reads the *previous* deadline (not the current counter), then advances it. This ensures:
  - No accumulation of latency jitter (unlike TVAL-based countdown)
  - Drifts at most one interval, even if the handler takes 5+ ms
- Calls `sched_wake_sleepers()` to promote sleeping tasks to READY if their sleep deadline has passed
- If `tick_callback` is set, calls it; else logs every 100 ticks to avoid UART spam
- **Called from IRQ context (critical section)**
- **Must NOT call any blocking functions or perform any I/O except UART**

### timer_set_callback(timer_callback_t cb)
```c
typedef void (*timer_callback_t)(void);
void timer_set_callback(timer_callback_t cb)
```

**Behavior:**
- Sets `tick_callback = cb`
- The callback will be invoked on every timer tick (inside timer_handle_irq)
- If callback is NULL, default debug logging resumes
- **Safe from any context** (volatile write)

### timer_get_frequency()
```c
uint64_t timer_get_frequency(void)
```

**Returns:** CNTFRQ_EL0 (physical timer frequency in Hz, typically 50,000,000 on ARM QEMU virt)

### timer_get_count()
```c
uint64_t timer_get_count(void)
```

**Returns:** Current value of CNTPCT_EL0 (physical timer counter, always ascending)
- **Note**: Reads the *physical* counter, not virtual, to avoid divergence under virtualization

### timer_get_ticks()
```c
uint64_t timer_get_ticks(void)
```

**Returns:** `tick_count` (number of timer interrupts since boot)
- Safe from any context (volatile read)

### timer_uptime_ms()
```c
uint64_t timer_uptime_ms(void)
```

**Returns:** `tick_count * TIMER_INTERVAL_MS`
- Elapsed time in milliseconds since boot
- Precision: ±TIMER_INTERVAL_MS due to 10 ms granularity

### timer_uptime_seconds()
```c
uint64_t timer_uptime_seconds(void)
```

**Returns:** `(tick_count * TIMER_INTERVAL_MS) / 1000`
- Elapsed time in seconds since boot (integer division, may lose sub-second precision)

## Register Reference (ARM Generic Timer)

All operations via inline assembly (MRS/MSR). All registers are 64-bit.

### CNTFRQ_EL0 (Counter Frequency, Read-Only)
```
[63:0]   Frequency in Hz
```
- Set by firmware before kernel starts
- Example on QEMU virt: 50,000,000 Hz (50 MHz)
- Cannot be changed by software

### CNTPCT_EL0 (Physical Counter, Read-Only)
```
[63:0]   Current count (always ascending, never resets)
```
- Used by CNTP_CVAL_EL0 comparison
- Increments at frequency CNTFRQ_EL0

### CNTP_CTL_EL0 (Physical Timer Control)
```
[0]      ENABLE (write 1 to enable, 0 to disable)
[1]      IMASK (write 1 to mask interrupt; not used here, left at 0)
[2]      ISTATUS (read-only; 1 if CNTPCT_EL0 >= CNTP_CVAL_EL0)
[63:3]   Reserved
```
- Used to enable/disable the physical timer
- IRQ fires when ENABLE=1 and CNTPCT_EL0 >= CNTP_CVAL_EL0

### CNTP_CVAL_EL0 (Physical Timer Compare Value)
```
[63:0]   Absolute deadline (compared against CNTPCT_EL0)
```
- When CNTPCT_EL0 reaches or exceeds this value, timer IRQ fires (if ENABLE=1)
- Must be set with an absolute future deadline
- Used for both initial arm and re-arming in IRQ handler

### CNTP_TVAL_EL0 (Physical Timer Value, alternative)
```
[63:0]   Signed countdown timer (decrements from initial value each tick)
```
- **NOT used in Fermi OS** (uses CVAL instead)
- Reason: TVAL-based countdown accumulates jitter; each IRQ latency shifts the next deadline

## Hardware Constants

```
GIC Physical Peripheral Interrupt ID 30 (PPI #30)
  - Delivered to all CPUs with CNTP_CTL_EL0[ENABLE]=1 and CNTPCT_EL0 >= CNTP_CVAL_EL0
  - GICv3 automatically EOI'd via gic_end_irq()

Default tick interval: 10 ms
  - At 50 MHz: 10 ms = 500,000 ticks
  - At 1 MHz: 10 ms = 10,000 ticks
```

## Subsystem Dependencies

### Depends On:
- **GIC (gic.h)**: `gic_enable_irq(TIMER_PPI_INTID)` to register the PPI, `gic_ack_irq()` and `gic_end_irq()` for IRQ delivery
- **Scheduler (sched.h)**: `sched_wake_sleepers()` to promote sleeping tasks on each tick
- **UART (uart.h)**: Debug logging (timer_init, timer_start, timer_stop, default tick logging)

### Depended On By:
- **Scheduler (sched.c)**: Ticks drive `schedule()`, which is called from exception_dispatch after EOI
- **Sleep syscalls (syscall.c)**: Tasks use `timer_get_ticks()` to wait until `tick_count >= sleep_until`
- **Network (net/net.c)**: Uses `timer_get_ticks()` for ping round-trip timing
- **Any subsystem needing precise timing**: Can read `timer_get_count()` (counter) or `timer_uptime_ms()`

## Boot / Usage Ordering

1. **exception.c / exceptions_init()**: Install VBAR_EL1 and exception handlers *before* timer_init
   - Timer relies on exception dispatch being ready to handle the first PPI
2. **gic.c / gic_init()**: Initialize GICv3 distributor and redistributor *before* timer_init
   - Timer calls gic_enable_irq() which requires GIC to be configured
3. **timer.c / timer_init()**: Read CNTFRQ_EL0, enable PPI in GIC
   - Single-threaded, early boot
4. **Scheduler / sched_init()**: Create idle task and initial tasks
   - Scheduler must exist before timer ticks can wake sleeping tasks
5. **timer.c / timer_start(TIMER_INTERVAL_MS)**: Arm the first deadline
   - Only after all subsystems are ready
6. **main / kernel.c**: Enter idle loop (`while(1) wfi`)
   - IRQs now begin delivery; each tick calls schedule()

Example from kernel.c:
```c
timer_init();                          // Reads frequency, enables PPI
timer_start(TIMER_INTERVAL_MS);        // Arms first deadline (10 ms from now)
uart_println("[KERNEL] Ready! running idle task...");
while (1) {
  __asm__ __volatile__("wfi");         // Sleep until IRQ; timer wakes us every 10 ms
}
```

## Volatile Statics and Synchronization

All four statics are **volatile** (compiler must not optimize away reads/writes) and are accessed from both task and IRQ context **without locks**:

```c
static volatile uint64_t timer_freq = 0;          // Written once at init, then read-only
static volatile uint64_t timer_interval = 0;      // Written at start, then read-only in IRQ
static volatile uint64_t tick_count = 0;          // Incremented only in IRQ, read from any context
static volatile timer_callback_t tick_callback = 0; // Set anytime, called from IRQ
```

**Synchronization notes:**
- `timer_freq` and `timer_interval` are set during boot before any IRQ can fire; no race
- `tick_count` is only *incremented* in IRQ context, never decremented; safe to read anytime (worst case: stale value)
- `tick_callback` is a function pointer; volatile ensures the load is not cached before each IRQ

No explicit memory barriers needed beyond volatile; the MRS/MSR for register access provides serialization.

## Struct Layouts

None. The timer subsystem does not expose any structs. It uses only primitive types and registers.

## Rust Port Strategy

### Module Structure
```rust
pub mod timer {
    // Private statics (volatile)
    static TIMER_FREQ: AtomicU64 = AtomicU64::new(0);
    static TIMER_INTERVAL: AtomicU64 = AtomicU64::new(0);
    static TICK_COUNT: AtomicU64 = AtomicU64::new(0);
    static TICK_CALLBACK: AtomicUsize = AtomicUsize::new(0); // usize for fn ptr

    // Public functions (same signatures)
    pub fn init()
    pub fn start(interval_ms: u64)
    pub fn stop()
    pub fn handle_irq()  // Called from exception.rs IRQ dispatch
    pub fn set_callback(cb: Option<TimerCallback>)
    pub fn get_frequency() -> u64
    pub fn get_count() -> u64
    pub fn get_ticks() -> u64
    pub fn uptime_ms() -> u64
    pub fn uptime_seconds() -> u64
}
```

### Key Decisions:
1. **Use `AtomicU64` instead of `volatile`**: Rust doesn't have volatile statics in the same way. `AtomicU64` with `Relaxed` ordering provides the same "no optimization" guarantee.
2. **No locks**: Like the C code, no synchronization needed beyond atomicity.
3. **Inline assembly for register access**: Use `asm!` macro for MRS/MSR, matching the C `__asm__ __volatile__`.
4. **Timer callback**: Store as `AtomicUsize` (fn ptr coerced to `usize`), call via transmute if Some.
5. **Dependency on exception + gic + sched**: Import via `use crate::exception::gic`, `use crate::sched`, `use crate::exception`.

### Which Parts Stay Assembly:
- **All register access (MRS/MSR)**: Must use inline `asm!` because Rust has no way to express system register operations. Examples:
  - `mrs %0, cntfrq_el0`
  - `mrs %0, cntpct_el0`
  - `msr cntp_cval_el0, %0`
  - `msr cntp_ctl_el0, %0`
  - These are NOT memory-mapped I/O; they are CPU registers, accessible only via special instructions.

## Critical Correctness Details & Gotchas

1. **CVAL vs TVAL**: Fermi uses CVAL (absolute deadline) exclusively. **Do NOT switch to TVAL** (countdown). Under high IRQ latency, TVAL jitter accumulates; CVAL drifts at most one interval per IRQ. This is a load-bearing design choice.

2. **Re-arm reads previous CVAL, not current count**: In `timer_handle_irq()`, the re-arm logic is:
   ```c
   uint64_t cval;
   __asm__ __volatile__("mrs %0, cntp_cval_el0" : "=r"(cval));
   cval += timer_interval;
   __asm__ __volatile__("msr cntp_cval_el0, %0" ::"r"(cval));
   ```
   This reads the *old* CVAL (which has now fired), not CNTPCT_EL0 (current count). This ensures the interval is measured between deadlines, not between "now and deadline". Critical for stability.

3. **Physical timer, not virtual**: Fermi uses CNTPCT_EL0 (physical counter) and CNTP_* registers, not CNTVCT_EL0 (virtual) or CNTV_*. Under virtualization, the virtual counter can diverge; physical is guaranteed to match across VMs.

4. **Frequency read-only at init**: CNTFRQ_EL0 is set by firmware and cannot be changed. Fermi assumes it doesn't change after init.

5. **PPI #30 is CPU-local**: Each CPU has its own physical timer and will fire PPI #30. On single-core systems, this is not an issue. On multi-core, each CPU sees the PPI independently (no affinity needed).

6. **Scheduler integration**: Every tick calls `sched_wake_sleepers()`, which wakes any task with `sleep_until <= tick_count`. Must be called *inside* IRQ context (before schedule()), not after. See exception.c for the IRQ dispatch order.

7. **No overflow handling**: `tick_count` will overflow after ~2^64 / 100 Hz ≈ 585 billion years. Not a concern for hobby OS.

8. **Memory barriers**: The MRS/MSR instructions for CNTP_* registers provide sufficient serialization. No explicit `isb` or `dsb` needed in the timer IRQ path (ISB is used elsewhere for VBAR, CPACR changes).

9. **Callback safety**: The callback is invoked from IRQ context. It must not:
   - Call any blocking functions (sleep, mutex)
   - Perform any I/O except emergency UART logging
   - Dereference pointers without verification
   - It *can* safely read `tick_count` and call `sched_wake_sleepers()` (already called anyway)

10. **Interrupt latency does NOT affect deadline**: Because CVAL is absolute, high interrupt latency before the handler runs does not shift the *next* deadline. The deadline is always measured from the previous deadline, not from "now". This is the core feature of using CVAL.

## Example: 10 ms Tick at 50 MHz

- CNTFRQ_EL0 = 50,000,000 Hz
- timer_interval = 50,000,000 * 10 / 1000 = 500,000 ticks
- Suppose CNTPCT_EL0 = 1,000,000,000 at timer_start():
  - Set CNTP_CVAL_EL0 = 1,000,000,000 + 500,000 = 1,000,500,000
  - First IRQ fires when CNTPCT_EL0 >= 1,000,500,000
  - timer_handle_irq() reads CNTP_CVAL_EL0 (now 1,000,500,000), adds 500,000
  - New CNTP_CVAL_EL0 = 1,001,000,000
  - tick_count = 1
  - Second IRQ fires when CNTPCT_EL0 >= 1,001,000,000
  - And so on, maintaining exactly 500,000-tick intervals (exactly 10 ms)

Even if the first IRQ handler took 5 ms (250,000 ticks), the second IRQ would fire at CNTPCT_EL0 >= 1,001,000,000 (still 500,000 ticks after the first), not earlier or later.

