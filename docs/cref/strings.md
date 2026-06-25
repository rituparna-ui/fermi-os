# Strings Subsystem Specification

## Overview

The strings subsystem provides fundamental memory manipulation and string formatting functions for the Fermi OS bare-metal aarch64 kernel. It is one of the earliest initialized subsystems—memset is called during zero_bss() in the boot sequence before any console output. The module exports low-level utilities:

- **Memory operations**: memcpy, memmove, memset, memcmp (byte-granular, no alignment requirements)
- **String operations**: strlen, strnlen, strcmp, strncmp, strncpy, strchr (C string utilities)
- **Formatted printing**: ksnprintf, kvsnprintf (snprintf-like buffer formatting)

These are used throughout the kernel: early BSS zeroing, panic dumps, device diagnostics, logging, and runtime string handling.

---

## Public API

### Memory Operations

#### memcpy
```c
void *memcpy(void *dest, const void *src, size_t n);
```
**Behavior:**
- Copies exactly `n` bytes from `src` to `dest`.
- Does NOT handle overlapping buffers safely (see memmove for overlap).
- Performs byte-by-byte copy in ascending order.
- Returns `dest`.

**Implementation detail:** Iterates `i` from 0 to n-1, copying `d[i] = s[i]`.

#### memmove
```c
void *memmove(void *dest, const void *src, size_t n);
```
**Behavior:**
- Copies exactly `n` bytes from `src` to `dest`, safe for overlapping buffers.
- If `dest == src` or `n == 0`, returns immediately.
- If `dest < src`, copies forward (ascending order).
- If `dest > src`, copies backward (descending order from n-1 to 0).
- Returns `dest`.

**Implementation detail:** Direction check prevents corruption when buffers overlap.

#### memset
```c
void *memset(void *dest, int c, size_t n);
```
**Behavior:**
- Fills `n` bytes of `dest` with the byte value `(uint8_t)c`.
- Returns `dest`.
- Used during boot in `zero_bss()` with c=0.

**Implementation detail:** Iterates `i` from 0 to n-1, setting `d[i] = (uint8_t)c`.

#### memcmp
```c
int memcmp(const void *a, const void *b, size_t n);
```
**Behavior:**
- Compares `n` bytes of memory buffers `a` and `b`.
- Returns 0 if all n bytes match.
- Returns `(int)a[i] - (int)b[i]` (as unsigned byte values cast to signed int) at first differing byte i.
- Follows POSIX memcmp semantics.

**Implementation detail:** Early-exit on first mismatch; returns difference of unsigned bytes.

### String Operations

#### strlen
```c
size_t strlen(const char *s);
```
**Behavior:**
- Returns the length of null-terminated string `s`, excluding the NUL terminator.
- Undefined behavior if `s` is not null-terminated.

#### strnlen
```c
size_t strnlen(const char *s, size_t maxlen);
```
**Behavior:**
- Returns the length of null-terminated string `s` up to `maxlen` bytes.
- Returns `maxlen` if no NUL byte is found within the first `maxlen` bytes.
- Safe variant for bounded string scanning.

#### strcmp
```c
int strcmp(const char *a, const char *b);
```
**Behavior:**
- Lexicographically compares null-terminated strings `a` and `b`.
- Returns 0 if strings are equal.
- Returns `(int)(uint8_t)*a - (int)(uint8_t)*b` at first differing character.
- Treats characters as unsigned bytes when computing difference.

#### strncmp
```c
int strncmp(const char *a, const char *b, size_t n);
```
**Behavior:**
- Lexicographically compares up to `n` characters of strings `a` and `b`.
- Returns 0 if first `n` characters match OR both strings are shorter than `n` and equal up to first NUL.
- Returns `(int)ca - (int)cb` (as unsigned bytes) at first differing character within n bytes.
- Stops early if a NUL is encountered.

**Implementation detail:** After finding a difference, returns immediately. If a byte is NUL, also returns 0 (strings match).

#### strncpy
```c
char *strncpy(char *dest, const char *src, size_t n);
```
**Behavior:**
- Copies up to `n` bytes from `src` to `dest`.
- Copies bytes from `src` until a NUL is found OR `n` bytes have been copied.
- Pads the remainder of `dest` (if `strlen(src) < n`) with NUL bytes to ensure deterministic content.
- Always returns `dest`.
- **Important**: If `strlen(src) >= n`, dest is NOT NUL-terminated (POSIX behavior).

**Implementation detail:**
  - First loop: copy src bytes until NUL or n bytes copied.
  - Second loop: pad remaining bytes with '\0' if we exited the first loop early.

#### strchr
```c
const char *strchr(const char *s, int c);
```
**Behavior:**
- Searches for the first occurrence of character `(unsigned char)c` in null-terminated string `s`.
- Returns pointer to the matching character if found.
- If `c` is NUL ('\0'), returns pointer to the NUL terminator (not NULL).
- Returns NULL if character is not found and is not NUL.

**Implementation detail:**
  - Scans forward using `*s` until a match or NUL is found.
  - Special case: target == 0 returns `s` (the position of NUL terminator).

### Formatted Printing

#### ksnprintf
```c
int ksnprintf(char *buf, size_t buflen, const char *fmt, ...);
```
**Behavior:**
- Printf-like formatting to a fixed-size buffer `buf` of capacity `buflen`.
- Returns the number of characters that would have been written (POSIX vsnprintf semantics), excluding the NUL terminator.
- Output is always NUL-terminated when `buflen > 0` (even if truncated).
- If `buflen == 0`, no output and buffer is not accessed.

**Supported format specifiers:**
- `%s`: const char * (NULL prints "(null)")
- `%d`: int (signed decimal, handles negative via 0 - value)
- `%u`: uint64_t (unsigned decimal)
- `%x`: uint64_t (lowercase hex, NO 0x prefix)
- `%c`: char (passed as int, extracted via default promotion)
- `%%`: literal percent character
- Unknown specifiers: pass through as "%X" (percent + specifier character)
- Lone trailing `%`: ignored (doesn't increment return count)

**Return value semantics:**
- Returns position where NUL was written (or would have been written if truncated).
- If string was truncated, return value still reflects the hypothetical full length.

#### kvsnprintf
```c
int kvsnprintf(char *buf, size_t buflen, const char *fmt, va_list args);
```
**Behavior:**
- Identical to ksnprintf but accepts pre-initialized va_list.
- Allows wrapper functions to pass varargs.

**Implementation detail:**
- Iterates through format string, dispatching on '%' specifiers.
- Uses internal helpers: buf_putc (character), buf_puts (string), buf_putu (unsigned integer conversion).

### Internal Helpers (Not Exported)

#### buf_putc
```c
static inline void buf_putc(char *buf, size_t buflen, size_t *pos, char c);
```
- Writes character `c` to `buf[*pos]` if there is room (`*pos + 1 < buflen`).
- **Always increments** `*pos` to track the output position, even if buffer is full.
- Used to ensure return value correctly reflects hypothetical output length.

#### buf_puts
```c
static void buf_puts(char *buf, size_t buflen, size_t *pos, const char *s);
```
- Writes all bytes of null-terminated string `s` using buf_putc.
- Respects buffer limits (truncation) but advances pos for each character.

#### buf_putu
```c
static void buf_putu(char *buf, size_t buflen, size_t *pos, uint64_t val, int base);
```
- Converts unsigned 64-bit integer `val` to string in given base (10 or 16).
- Uses static table: `"0123456789abcdef"`.
- Writes in correct order using a temporary stack buffer `tmp[24]`.
- Special case: if `val == 0`, writes '0'.
- Base <= 16 (caller ensures correctness).

---

## Hardware Constants & Memory Layout

No hardware registers. No memory-mapped I/O.

All functions operate on general memory buffers passed by caller. Alignment constraints:
- **No alignment required** for any input/output buffers (functions iterate byte-by-byte).
- Operations are naturally thread-safe for non-overlapping ranges.

---

## Boot & Usage Ordering

### Initialization Phase
1. **zero_bss()** (kernel.c, early_init)
   - Calls `memset(&__bss_start, 0, size_of_bss)` to zero BSS segment.
   - **Called before UART is initialized** — must not depend on console output.
   - Happens during PAS (Physical Address Space) execution.

2. **UART and logging initialization**
   - After zero_bss, UART is initialized.
   - ksnprintf and va_list-based formatting become available for diagnostic output.

3. **Panic system**
   - panic/panic.c uses uart_printf (which does NOT use ksnprintf internally).
   - panic.c directly calls uart_putc/uart_puts and manual formatting.

### Runtime Usage
- **Kernel**: Used by all subsystems for memory operations and diagnostics.
- **Panic dumps**: System registers are logged using uart_printf.
- **File systems, networking, devices**: Generic string ops for parsing and formatting.

---

## Rust Porting Strategy

### Module Structure

```
fermi_kernel
├── strings/
│   ├── mod.rs                 (re-exports, public API)
│   ├── mem.rs                 (memory operations)
│   ├── str.rs                 (string operations)
│   └── fmt.rs                 (formatting, internal helpers)
```

### Core Design Principles

1. **No_std only**: Use core::ptr, core::slice. No alloc required.
2. **Zero copy where possible**: Accept `&[u8]`, `&mut [u8]`, `&str` using Rust's type safety.
3. **Unsafe where needed**: memcpy/memmove must use `ptr::copy` or manual volatile access for overlaps.
4. **Replace with Rust builtins where available**:
   - memcpy → compiler_builtins or core::ptr::copy_nonoverlapping
   - memmove → core::ptr::copy (handles overlaps)
   - memset → core::slice iteration (or volatile writes for hardware registers)
   - strlen → &str.len() (but keep C-string variant for compatibility)
5. **Exact semantics preservation**: Return values, signedness, truncation behavior must match C exactly.

### Type Mapping

| C Type        | Rust Type                       | Notes                              |
|---------------|---------------------------------|------------------------------------|
| void *        | *mut u8                         | Generic pointer as byte ptr        |
| const void *  | *const u8                       | Read-only byte ptr                 |
| int           | i32                             | Fixed 32-bit signed                |
| uint64_t      | u64                             | Fixed 64-bit unsigned              |
| size_t        | usize                           | Platform-native pointer-sized      |
| const char *  | *const u8, &CStr, or &str       | Context-dependent                  |

### Module: mem.rs (Memory Operations)

```rust
// pub fn memcpy(dest: *mut u8, src: *const u8, n: usize) -> *mut u8
// pub fn memmove(dest: *mut u8, src: *const u8, n: usize) -> *mut u8
// pub fn memset(dest: *mut u8, c: u32, n: usize) -> *mut u8
// pub fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32
```

**Strategy:**
- Use `ptr::copy_nonoverlapping` for memcpy (LLVM knows this doesn't overlap).
- Use `ptr::copy` for memmove (LLVM knows this handles overlaps).
- Loop + volatile write for memset (to prevent compiler from optimizing).
- Iterate byte pairs for memcmp, return signed difference of unsigned bytes.
- Return pointers correctly (dest for all, difference for cmp).

### Module: str.rs (String Operations)

```rust
// pub fn strlen(s: *const u8) -> usize
// pub fn strnlen(s: *const u8, maxlen: usize) -> usize
// pub fn strcmp(a: *const u8, b: *const u8) -> i32
// pub fn strncmp(a: *const u8, b: *const u8, n: usize) -> i32
// pub fn strncpy(dest: *mut u8, src: *const u8, n: usize) -> *mut u8
// pub fn strchr(s: *const u8, c: i32) -> *const u8
```

**Strategy:**
- Work with raw pointers (C ABI compatibility).
- strlen: Loop until null byte found, return count.
- strnlen: Loop until null byte OR maxlen reached.
- strcmp/strncmp: Iterate, cast to unsigned u8 for subtraction (sign-extended to i32).
- strncpy: Copy loop + padding loop (preserve POSIX semantics).
- strchr: Return pointer to match or null (handle c == 0 case as NUL terminator).

### Module: fmt.rs (Formatted Printing)

```rust
// pub fn ksnprintf(buf: *mut u8, buflen: usize, fmt: *const u8, ...) -> i32
// pub fn kvsnprintf(buf: *mut u8, buflen: usize, fmt: *const u8, args: va_list) -> i32
// (internal) fn buf_putc(buf: *mut u8, buflen: usize, pos: &mut usize, c: u8)
// (internal) fn buf_puts(buf: *mut u8, buflen: usize, pos: &mut usize, s: *const u8)
// (internal) fn buf_putu(buf: *mut u8, buflen: usize, pos: &mut usize, val: u64, base: i32)
```

**Strategy:**
- Use Rust va_list from core::ffi::VaList or libc::va_list.
- Loop through format string, dispatch on '%' byte.
- Track position (pos) as mutable usize; always increment pos even if buffer full.
- buf_putc: Conditional write, unconditional pos increment.
- buf_puts: Call buf_putc per byte.
- buf_putu: Extract digits into local [u8; 24], then reverse-write to buffer.
- Specifiers: %s (null → "(null)"), %d (signed via negation), %u (unsigned), %x (hex, base 16), %c (char from int), %% (literal %), unknown (pass through).

### Locking & Concurrency

- **No locking needed**: All functions are pure (no static state except literal digit table).
- Functions operate on caller-provided buffers; caller ensures no data races.
- ksnprintf uses static const `DIGITS` table (read-only).

### Static Data

```rust
// In fmt.rs
const DIGITS: &[u8] = b"0123456789abcdef";
```

### What Must Stay in Assembly

None—pure Rust can handle all operations. However:
- If volatile writes are needed for memset (to prevent optimization), use `core::ptr::write_bytes` or explicit volatile writes.
- C-level volatile semantics are not required unless specific hardware write barriers are needed (they are not for this subsystem).

---

## Exact Semantics & Edge Cases

### memcpy
- **No overlap handling**: Behavior undefined if dest and src overlap.
- **Return value**: Always dest (returned as-is).
- **n=0**: Valid; copies 0 bytes, returns dest.
- **Null pointers**: If n > 0 and either pointer is null, behavior is undefined (caller must validate).

### memmove
- **Overlap handling**: Bidirectional check ensures safe copies.
- **If dest == src**: Returns immediately with no copy.
- **If n == 0**: Returns immediately.
- **Backward copy**: When dest > src, iterates from n-1 down to 0 to prevent corruption.

### memset
- **Value truncation**: int c is cast to uint8_t; only low 8 bits are used.
- **n=0**: Valid; fills 0 bytes, returns dest.
- **Return value**: Always dest.

### memcmp
- **Signedness**: Comparison uses unsigned byte values, cast to signed int for difference.
- **Early exit**: Returns at first differing byte.
- **All match**: Returns 0.
- **Return range**: [-255, 255] (one byte difference max).

### strlen
- **Unterminated string**: Undefined behavior; function will scan until a random null byte.
- **Empty string**: strlen("") returns 0.

### strnlen
- **Safe boundary**: Stops at maxlen OR null byte, whichever comes first.
- **No null within maxlen**: Returns maxlen (not an error).

### strcmp
- **Signedness**: Like memcmp, differences are (unsigned byte a) - (unsigned byte b), cast to signed int.
- **NUL termination**: Both strings must be null-terminated; function stops at first NUL in either.

### strncmp
- **Early exit on NUL**: If either string has a NUL before n bytes, comparison stops and returns 0 (if both strings match up to that point).
- **No NUL within n**: Behavior depends on whether buffers are overread (caller must ensure valid memory).

### strncpy
- **Padding**: If src is shorter than n, dest is padded with NULs.
- **No termination if truncated**: If strlen(src) >= n, dest[0..n-1] contains src bytes and dest is NOT null-terminated (POSIX behavior).
- **Caller responsibility**: After calling strncpy with src longer than n, caller must manually null-terminate if needed.

### strchr
- **NUL search**: If c == 0 (NUL character), returns pointer to the NUL terminator, not NULL.
- **Not found**: Returns NULL if character is not in string and is not NUL.
- **Return type**: const char *, so cast to *const u8 in Rust.

### ksnprintf / kvsnprintf

**Truncation behavior:**
- If output exceeds buflen-1 characters, excess is discarded but pos counter continues.
- Return value is the total length that would have been written (POSIX vsnprintf semantics).
- NUL terminator is always written (at min(pos, buflen-1)) when buflen > 0.

**Format specifier details:**
- `%s`: If pointer is null, outputs "(null)". If non-null, outputs all bytes until NUL.
- `%d`: va_arg extracts int; if negative, outputs minus sign then buf_putu(-(uint64_t)val).
  - **Edge case**: INT_MIN → -(uint64_t)INT_MIN requires careful unsigned arithmetic.
- `%u`: va_arg extracts uint64_t; outputs in decimal.
- `%x`: va_arg extracts uint64_t; outputs in lowercase hex (16), no 0x prefix.
- `%c`: va_arg extracts int; cast to char (single byte output).
- `%%`: Outputs single '%' character.
- Unknown `%X`: Outputs '%' followed by the unknown character.
- Trailing `%`: If format ends with '%', ignored (pos not incremented for that char).

**Position tracking:**
- pos starts at 0.
- Each buf_putc increments pos, regardless of success.
- Final pos value is returned.
- NUL terminator is written at buf[pos < buflen ? pos : buflen - 1] when buflen > 0.

---

## Gotchas & Subtle Issues

1. **memcpy with overlapping buffers**: C memcpy behavior is undefined; must use memmove.
2. **strcmp/strncmp signedness**: Difference computed from unsigned bytes cast to int; matches C semantics exactly.
3. **strncpy no-termination case**: Easy to forget that strncpy does NOT null-terminate if src >= n; not a buffer overflow but a trap.
4. **ksnprintf truncation return value**: Return is the hypothetical full length, not the actual bytes written. Caller must check if return >= buflen to detect truncation.
5. **va_list portability**: Must use correct va_list type from C ABI (libc::va_list or core::ffi::VaList).
6. **buf_putc always advances pos**: This is deliberate—ensures return value is correct even on truncation.
7. **strchr(s, '\0')**: Returns pointer to NUL, not NULL. Callers expecting NULL on not-found must check explicitly.
8. **memcmp byte order**: Treated as simple byte sequences; no endianness considerations (byte-by-byte comparison).
9. **memmove backward iteration**: When dest > src, must iterate from n-1 downward to prevent self-corruption; forward iteration would overwrite source before it's read.
10. **Integer overflow in memcpy/memmove**: If n is extremely large (near usize::MAX), no protection; caller must validate ranges.

---

## Dependencies & Call Graph

### Subsystem Dependencies
- **Depended on by**: Nearly all other subsystems (memcpy in MMU init, memset in BSS zeroing, strlen/strcmp for string operations, ksnprintf for logging throughout kernel).
- **Depends on**: None (no other kernel subsystems).
- **External dependencies**: Only core Rust (no alloc, no std, no libc beyond va_list for formatting).

### Boot Sequence Dependency
- zero_bss() calls memset during early_init (before console is available).
- This must work with no initialization overhead.

---

## Summary

The strings subsystem is a foundational utility library providing:
- Byte-level memory operations (copy, move, set, compare)
- C string utilities (length, comparison, search, copy)
- Printf-like formatting to fixed buffers

All functions are non-allocating, reentrant, and work on caller-provided buffers. The Rust port should preserve exact C semantics (signedness, truncation, NUL handling) while leveraging Rust's type safety where possible (slices, immutability). No assembly required; all logic can be implemented safely in Rust using core::ptr and core::ffi::VaList.

