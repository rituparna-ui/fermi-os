# User Subsystem (EL0) — Porting Specification

**Subsystem Key:** `user`

## Overview

The user subsystem enables execution of userspace programs at Exception Level 0 (EL0) on the ARM64 architecture. It provides:

1. **EL0 program execution model** — loaded ELF64 binaries, mapped at USER_TEXT_BASE with independent page tables (TTBR0)
2. **Syscall ABI** — AAPCS64 convention with x8 = syscall number, x0–x7 = arguments, x0 = return value
3. **User-space memory layout** — contiguous VA range [0, USER_STACK_TOP) with code, data, bss, and stack
4. **Runtime startup** — crt0 stub that receives argc/argv in x0/x1 and converts main()'s return to SYS_EXIT
5. **User libc header** — sys.h with inline syscall wrappers and minimal string helpers (u_strlen, u_puts)
6. **Program loading** — kernel loads ELF64 segments with permission mapping (PT_LOAD p_flags → PTE access bits)

## Hardware & Architecture Constants

### Memory Layout

| Component | Address | Size | Notes |
|-----------|---------|------|-------|
| USER_TEXT_BASE | 0x00400000 | varies | User code entry point; ELF segments loaded here |
| USER_STACK_TOP | 0x00800000 | 16 KiB (4 pages) | Top of user stack; grows downward |
| USER_STACK_PAGES | 4 | 4 KiB each | Stack allocation quantum |

**User address range:** [0x0000000000000000, 0x0000000000800000) — all below USER_STACK_TOP

### Syscall Constants (x8 register)

```
SYS_READ        0   read(fd, buf, len) → ssize_t
SYS_WRITE       1   write(fd, buf, len) → ssize_t
SYS_OPEN        2   open(path) → int
SYS_CLOSE       3   close(fd) → int
SYS_EXIT        4   exit(code) — never returns
SYS_YIELD       5   yield() → int (always 0)
SYS_SLEEP       6   sleep(ms) → void
SYS_GETPID      7   getpid() → int
SYS_LSEEK       8   lseek(fd, offset, whence) → int64_t
SYS_UPTIME      9   uptime() → uint64_t (milliseconds since boot)
SYS_NET_PING    10  net_ping(seq) → int (TTL or -1)
SYS_KILL        11  kill(pid) → int (0 or -1)
SYS_FORK        12  fork() → int (child pid to parent, 0 to child)
SYS_EXEC        13  exec(path, argv) → int (never returns on success; -1 on failure)
SYS_BALLOON     14  balloon(op, n) → int64_t (op-specific return)
```

### File Descriptor Constants

```
STDIN_FILENO   0   Standard input (/dev/console)
STDOUT_FILENO  1   Standard output (/dev/console)
STDERR_FILENO  2   Standard error (/dev/console)
```

### Seek Constants (SYS_LSEEK whence parameter)

```
SEEK_SET  0   Absolute offset
SEEK_CUR  1   Offset from current position
SEEK_END  2   Offset from end of file
```

### Balloon Sub-operations (SYS_BALLOON op parameter)

```
BALLOON_OP_INFLATE  0   Inflate balloon by n pages
BALLOON_OP_DEFLATE  1   Deflate balloon by n pages
BALLOON_OP_ACTUAL   2   Get actual balloon size (ignores n)
BALLOON_OP_TARGET   3   Get host-requested target size (ignores n)
```

### PTE (Page Table Entry) Bits & Flags

**Access Permission Bits (AP[7:6])**
```
PTE_AP_RW       (0ULL << 6)   EL1 RW, EL0 no access
PTE_AP_RW_EL0   (1ULL << 6)   EL1 RW, EL0 RW
PTE_AP_RO       (2ULL << 6)   EL1 RO, EL0 no access
PTE_AP_RO_EL0   (3ULL << 6)   EL1 RO, EL0 RO
```

**Execution Bits**
```
PTE_UXN         (1ULL << 54)  User cannot execute (data pages)
PTE_PXN         (1ULL << 53)  Kernel cannot execute (user code)
PTE_NG          (1ULL << 11)  Non-Global: TLB entries tagged by ASID
```

**Descriptor Type & Flags**
```
PTE_VALID       (1ULL << 0)   Valid entry (not invalid/reserved)
PTE_TABLE       (1ULL << 1)   Table descriptor (intermediate level)
PTE_AF          (1ULL << 10)  Access Flag (raise fault if 0 on use)
PTE_SH_INNER    (3ULL << 8)   Inner shareable memory
PTE_ATTRIDX(idx) ((idx) << 2)  Memory attribute index from MAIR_EL1
```

**Physical Address Mask**
```
PTE_ADDR_MASK   0x0000FFFFFFFFF000ULL  4 KiB granule, 48-bit PA field
```

**L3 Index Extraction**
```
L3_INDEX(va)    (((va) >> 12) & 0x1FF)  Extract L3 page table index
```

**TTBR0_EL1 Layout (when AS=1, A1=0)**
```
TTBR_ASID_SHIFT          48        ASID bits [63:48]
TTBR_BADDR_MASK          0x0000FFFFFFFFFFFFULL  Page table base [47:1]
ttbr_pack(baddr, asid)   ((baddr & TTBR_BADDR_MASK) | ((uint64_t)asid << 48))
```

### Trap Frame Layout

The trap frame (passed to syscall_dispatch as trap_frame_t*) occupies 688 bytes total on the stack:

**Register offsets (64-bit values)**
- `regs[0]` — x0 (arg0 / return value)
- `regs[1]` — x1 (arg1)
- `regs[2]` — x2 (arg2)
- `regs[8]` — x8 (syscall number)
- `regs[0..30]` — x0–x30
- `elr` — Exception Link Register (PC to return to)
- `spsr` — Saved Program Status Register
- **offset 280** — sp_el0 (user stack pointer) — NOT exposed in trap_frame_t struct; access via `uint64_t *frame_raw[35]`

**Initialization on SYS_EXEC success**
- All `regs[0..30]` cleared to 0
- `regs[0]` = argc
- `regs[1]` = argv pointer (user VA)
- `regs[2]` = envp (NULL)
- `elr` = img.entry (user program entry point, typically 0x400000)
- `spsr` = 0 (EL0t, IRQs unmasked)
- `frame_raw[35]` (sp_el0) = USER_STACK_TOP or aligned argv base

## Public API

### Syscall Wrappers (user/include/sys.h)

All syscall wrappers use inline GCC asm with AAPCS64 calling convention. Register constraints:
- Input constraints: x0..x2, x8 loaded with arguments
- Memory clobber: all wrappers clobber "memory" to prevent optimization across syscall
- Output/modify: x0 contains return value

#### ssize_t sys_read(int fd, void *buf, size_t count)
Read up to `count` bytes from file descriptor `fd` into `buf`. Returns bytes read (0 = EOF, -1 = error).

```c
register long x0 __asm__("x0") = fd;
register void *x1 __asm__("x1") = buf;
register size_t x2 __asm__("x2") = count;
register uint64_t x8 __asm__("x8") = SYS_READ;
__asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
return (ssize_t)x0;
```

#### ssize_t sys_write(int fd, const void *buf, size_t count)
Write `count` bytes from `buf` to file descriptor `fd`. Returns bytes written (-1 = error).

```c
register long x0 __asm__("x0") = fd;
register const void *x1 __asm__("x1") = buf;
register size_t x2 __asm__("x2") = count;
register uint64_t x8 __asm__("x8") = SYS_WRITE;
__asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
return (ssize_t)x0;
```

#### int sys_open(const char *path)
Open file at `path`. Returns file descriptor (>= 0) or -1 on error.

```c
register const char *x0 __asm__("x0") = path;
register uint64_t x8 __asm__("x8") = SYS_OPEN;
__asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
return (int)(long)x0;
```

#### int sys_close(int fd)
Close file descriptor `fd`. Returns 0 on success, -1 on error.

```c
register long x0 __asm__("x0") = fd;
register uint64_t x8 __asm__("x8") = SYS_CLOSE;
__asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
return (int)x0;
```

#### void sys_exit(void)
Terminate the calling process with implicit code 0. Never returns; kernel kills task.

```c
register uint64_t x8 __asm__("x8") = SYS_EXIT;
__asm__ __volatile__("svc #0" ::"r"(x8) : "memory");
__builtin_unreachable();
```

#### int sys_getpid(void)
Get the process ID of the calling task. Returns PID (>= 0).

```c
register long x0 __asm__("x0");
register uint64_t x8 __asm__("x8") = SYS_GETPID;
__asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
return (int)x0;
```

#### int64_t sys_lseek(int fd, int64_t off, int whence)
Seek to offset `off` in file `fd` relative to `whence` (SEEK_SET/CUR/END). Returns new absolute offset or -1 on error.

```c
register long x0 __asm__("x0") = fd;
register int64_t x1 __asm__("x1") = off;
register long x2 __asm__("x2") = whence;
register uint64_t x8 __asm__("x8") = SYS_LSEEK;
__asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
return (int64_t)x0;
```

#### uint64_t sys_uptime(void)
Get kernel uptime in milliseconds since boot. Returns uint64_t milliseconds.

```c
register uint64_t x0 __asm__("x0");
register uint64_t x8 __asm__("x8") = SYS_UPTIME;
__asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
return x0;
```

#### void sys_sleep(uint64_t ms)
Sleep for approximately `ms` milliseconds. Returns (implicitly void).

```c
register uint64_t x0 __asm__("x0") = ms;
register uint64_t x8 __asm__("x8") = SYS_SLEEP;
__asm__ __volatile__("svc #0" ::"r"(x0), "r"(x8) : "memory");
```

#### size_t u_strlen(const char *s)
Count bytes in NUL-terminated string `s` (not including NUL). Pure C, no syscall.

```c
size_t n = 0;
while (s[n]) n++;
return n;
```

#### void u_puts(const char *s)
Write NUL-terminated string `s` to stdout (fd 1). Implemented as `sys_write(STDOUT_FILENO, s, u_strlen(s))`.

## Struct Layouts

### elf_image_t

Kernel-side struct tracking ELF load metadata. Initialized by elf_load(), passed through SYS_EXEC.

```c
typedef struct {
  uint64_t entry;              // Entry point VA (typically 0x400000)
  int region_count;            // Number of PT_LOAD regions mapped
  // (additional fields not exposed to user but tracked by kernel)
} elf_image_t;
```

**Constants**
- `EXEC_MAX_BYTES` = 1 MiB — max user binary size
- `EXEC_MAX_ARGC` = 32 — max argument count
- `EXEC_ARG_BYTES_MAX` = 1024 — total argv string bytes budget

## Boot & Execution Flow

### 1. User Program Compilation & Linking

**Assembly programs (e.g., hello.S)**
```bash
aarch64-linux-gnu-gcc -ffreestanding -nostartfiles -nostdlib -fno-pic -static \
  -Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
  -o hello.elf hello.S
```

**C programs (e.g., cat.c)**
```bash
# First build crt0.o
aarch64-linux-gnu-gcc -ffreestanding -nostartfiles -nostdlib -fno-pic -c \
  -o user/lib/crt0.o user/lib/crt0.S

# Then link C program with crt0
aarch64-linux-gnu-gcc -ffreestanding -nostartfiles -nostdlib -fno-pic -static \
  -Wall -Wextra -O0 -g -I user/include \
  -Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
  -o cat.elf user/lib/crt0.o user/cat.c
```

**Linker script** (implicit via `-Wl,-Ttext=0x400000`): All sections map to VA 0x400000; kernel loads as ELF (not flat binary).

### 2. Kernel SYS_EXEC Entry

User program calls:
```c
const char *argv[] = { "cat", "file.txt", NULL };
sys_exec("/mnt/fat32/CAT.ELF", argv);
```

Kernel syscall_dispatch() sees num=13 (SYS_EXEC), calls sys_exec(arg_path, arg_argv, frame).

### 3. ELF Load Phase (sys_exec in src/syscall/syscall.c)

1. **Validate user pointers** — arg_path and arg_argv must lie in [0, USER_STACK_TOP)
2. **Capture argv** — while old TTBR0 still active, copy argv strings to kernel scratch buffer (arg_kbuf[1024])
3. **Open + read binary** — VFS fd_open(), fd_seek(SEEK_END), fd_read() into kernel heap
4. **Size check** — binary must be <= 1 MiB
5. **Allocate new user stack** — USER_STACK_PAGES (4 pages = 16 KiB) of physical pages
6. **Create user L0 page table** — mmu_create_user_tables() allocates L0, L1, L2, L3 tables
7. **Load PT_LOAD segments** — elf_load() walks ELF header, maps each PT_LOAD:
   - Allocates PMM pages for segment body
   - Copies segment bytes from kbuf
   - Zero-fills .bss gap (memsz > filesz)
   - Maps with flags derived from p_flags:
     - PF_X + PF_R → RO + EL0-X (code)
     - PF_W + PF_R → RW + UXN (data, bss)
     - PF_R (no W, no X) → RO + UXN (rodata)
   - Returns on failure; elf_load frees partial allocations
8. **Map stack** — mmu_map_user_range() maps stack_phys (RW + UXN) at [USER_STACK_TOP - 16 KiB, USER_STACK_TOP)

### 4. Stack Building

For argc > 0:
- **String blob** — all argv strings copied to top of user stack, growing downward
- **argv array** — array of (argc + 1) pointers to user VAs of strings, 16-byte aligned
- **SP_EL0** — 16-byte aligned below argv array, points to empty stack space

Layout (high to low):
```
USER_STACK_TOP                    (8 MB)
[argv[0] string bytes]            (e.g., "cat")
[argv[1] string bytes]            (e.g., "file.txt")
<16-byte alignment pad>
argv[argc] = NULL (8 bytes)
argv[argc-1] = 0x...             (pointer to argv[1] string)
argv[argc-2] = 0x...             (pointer to argv[0] string)
<16-byte alignment pad>
SP_EL0 ← trap frame sp_el0        (starts here, grows downward)
```

### 5. TTBR0 Swap & Trap Frame Rewrite

1. **Allocate ASID** — sched_asid_alloc() reserves a fresh 16-bit ASID
2. **Pack TTBR0** — ttbr_pack(new_l0_paddr, new_asid)
3. **MSR TTBR0_EL1** — activate new translation table + ASID
4. **ISB** — instruction synchronization barrier
5. **Rewrite trap frame**
   - x0 = argc
   - x1 = argv_user_base (VA of argv[0] pointer)
   - x2 = 0 (envp)
   - x3..x30 = 0
   - elr = img.entry (0x400000)
   - spsr = 0 (EL0t, IRQs unmasked)
   - sp_el0 = USER_STACK_TOP or aligned argv base
6. **Free old image** — after TTBR0 changed, ASID invalidated, pmm_free_pages() old stack + text

### 6. ERET & Program Entry

ERET loads:
- PC ← elr (0x400000, user _start)
- SP_EL0 ← sp_el0 value (16-byte aligned below argv)
- x0 = argc
- x1 = argv pointer
- x2 = 0
- EL ← EL0, IRQs unmasked

### 7. C Runtime (crt0.S)

**Entry (_start in user/lib/crt0.S)**
```asm
.globl _start
_start:
    bl   main                        ; call main(argc, argv, envp=NULL)
    mov  x8, #4                      ; SYS_EXIT
    svc  #0                          ; never returns
    1: wfe
    b    1b                          ; defensive halt
```

**AAPCS64 calling convention**
- Arguments already in x0 (argc), x1 (argv), x2 (envp)
- `bl main` calls main(); return value in x0
- SVC #0 with x8=4 invokes SYS_EXIT

### 8. User main() Execution

**C program (e.g., cat.c)**
```c
int main(int argc, char **argv) {
  // argc = 2, argv[0] = "cat", argv[1] = "file.txt", argv[2] = NULL
  // ...
  return 0;
}
```

**Assembly program (e.g., hello.S)**
```asm
_start:
    mov x0, #1                       ; fd = 1 (stdout)
    adr x1, msg                      ; buf = msg (PC-relative)
    mov x2, #(msg_end - msg)         ; len
    mov x8, #1                       ; SYS_WRITE
    svc #0
    mov x8, #4                       ; SYS_EXIT
    svc #0
```

## Validation & Bounds Checking

User pointer validation is critical for kernel safety. All syscall paths validate:

### user_buf_ok(uint64_t ptr, size_t len)
Returns 1 iff [ptr, ptr+len) lies entirely inside [0, USER_STACK_TOP):
- Zero-length buffers allowed (ptr can be anything)
- Overflow guard: ptr + len must not wrap
- Upper bound: ptr + len <= USER_STACK_TOP

```c
if (len == 0) return 1;
if (ptr + len < ptr) return 0;      // overflow
if (ptr + len > USER_STACK_TOP) return 0;
return 1;
```

### user_str_ok(uint64_t ptr)
Returns string length (excluding NUL) iff string is NUL-terminated within USER_PATH_MAX (4096):
- Boundary: ptr < USER_STACK_TOP
- Search limit: min(USER_STACK_TOP - ptr, USER_PATH_MAX)
- Finds NUL terminator or returns -1

```c
if (ptr >= USER_STACK_TOP) return -1;
uint64_t bound = USER_STACK_TOP - ptr;
if (bound > USER_PATH_MAX) bound = USER_PATH_MAX;
const char *s = (const char *)ptr;
for (uint64_t i = 0; i < bound; i++) {
  if (s[i] == '\0') return (int64_t)i;
}
return -1;
```

## Rust Porting Strategy

### Module Structure

```
fermi_kernel::user
├── mod.rs                  # Module root + exports
├── syscall.rs              # Syscall dispatcher + individual syscall handlers
├── elf.rs                  # ELF loader (ElfImage, LoadState)
├── startup.rs              # crt0 logic (startup sequence, entry frame setup)
└── consts.rs               # All public constants (USER_TEXT_BASE, SYS_*, etc.)

fermi_kernel::user_lib (or bundled in docs)
└── sys.rs (generated)      # User-space syscall wrappers (published as header)
```

### Type Design

**Syscall Constants (consts.rs)**
```rust
pub const USER_TEXT_BASE: u64 = 0x0040_0000;
pub const USER_STACK_TOP: u64 = 0x0080_0000;
pub const USER_STACK_PAGES: usize = 4;
pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
pub const SYS_READ: u64 = 0;
// ... etc
```

**ELF Image (elf.rs)**
```rust
pub struct ElfImage {
    pub entry: u64,
    pub region_count: usize,
    // phys_ranges: Vec<(u64, u64)>,  // tracked for cleanup
}

pub struct LoadState {
    l0_page_table: *mut u64,
    phys_pages: Vec<(u64, usize)>,  // (phys_addr, num_pages) for cleanup
}

impl LoadState {
    pub fn load_elf(kbuf: &[u8], new_l0: *mut u64) -> Result<ElfImage, i32>;
    pub fn cleanup(&mut self);
}
```

**Trap Frame (from exception subsystem, reference here)**
```rust
#[repr(C)]
pub struct TrapFrame {
    pub regs: [u64; 31],  // x0..x30
    pub elr: u64,
    pub spsr: u64,
    // sp_el0 at offset 280 (frame_raw[35])
}
```

**Argv Stack Builder (syscall.rs)**
```rust
struct ArgvBuilder {
    arg_kbuf: Vec<u8>,           // arg strings copied from user
    arg_offsets: Vec<usize>,     // offsets of each argv string
    argc: usize,
    stack_kbase: u64,            // kernel VA of freshly-allocated stack
    stack_user_lo: u64,          // user VA of stack base
}

impl ArgvBuilder {
    fn validate_from_user(&mut self, arg_argv_ptr: u64) -> Result<(), i32>;
    fn build_on_stack(&mut self, user_stack_top: u64) -> (u64, u64, u64);  // (argc_val, argv_ptr, sp_el0)
}
```

### Locking & Concurrency Strategy

- **No internal Mutex for syscall dispatch** — syscall_dispatch() called from exception handler with interrupts disabled; uses per-task fd_table (locked elsewhere)
- **TTBR0 swap atomic** — MSR + ISB prevents interleaving; task must not yield between swap and frame rewrite
- **ASID allocation** — uses scheduler's asid_alloc() (assumes atomic or spinlock-protected)
- **PMM allocations** — use scheduler's pmm::allocate_pages (already has locking)

### Assembly Requirements

**Parts that MUST stay in inline asm:**
1. **SVC #0 invocation in sys_* wrappers** — cannot be abstracted; inline asm with register constraints required for AAPCS64
2. **TTBR0_EL1 MSR + ISB in SYS_EXEC** — MMU register access must be inline; ISB synchronization barrier is critical
3. **TLBI (TLB Invalidate) in ASID cleanup** — tlbi aside1 is privileged; must be inline asm
4. **ERET epilogue** — returns to EL0; must be arch-specific

**Can be Rust:**
- Syscall number dispatch switch
- Argument validation (user_buf_ok, user_str_ok)
- ELF header walking and segment mapping
- Page table traversal for mmu_map_user_range
- Trap frame rewriting
- argv copying & stack layout

### core::arch Features Used

```rust
use core::arch::asm;

// In sys_read/write/etc wrappers:
let ret: i64;
unsafe {
    asm!(
        "svc #0",
        in("x0") arg0,
        in("x1") arg1,
        in("x2") arg2,
        in("x8") syscall_num,
        inout("x0") ret,
        options(nostack, readonly)
    );
}
```

### No alloc Dependencies

- User-space sys.h header (for user programs) is pure C, standalone
- Kernel-side loader CAN use alloc (heap, Vec, etc.) because it runs at EL1 and can allocate/free
- Exception handler must be no_std (no allocations during syscall dispatch)

## Gotchas & Subtleties

1. **ASID Invalidation Before PMM Free**
   - After TTBR0 swap, old entries remain in TLB tagged with old ASID
   - Must TLBI ASIDE1 with old ASID before freeing old page tables
   - Order: tlbi aside1 → dsb ish → free old_l0

2. **sp_el0 Not in trap_frame_t Struct**
   - sp_el0 stored at frame[280] on-stack, outside the C struct
   - SYS_EXEC must rewrite as `uint64_t *frame_raw = (uint64_t *)frame; frame_raw[35] = new_sp;`
   - ERET restores sp_el0 from this location

3. **argv Stack Layout Must Be 16-byte Aligned**
   - AAPCS64 requires SP_EL0 16-byte aligned on entry
   - Strings grow downward from USER_STACK_TOP
   - argv array sits 16-byte aligned below strings
   - sp_el0 sits 16-byte aligned below argv array
   - Compute: `sp_el0 = (argv_user_base - stride) & ~15`

4. **User Pointer Injection Prevention**
   - All syscall arguments from user are pointers/buffers → validate with user_buf_ok / user_str_ok
   - Not validating = kernel can read/write arbitrary kernel VA on behalf of user
   - Validation range is [0, USER_STACK_TOP) — entire lower VA space
   - Zero-length buffers (len=0) are always valid (no dereference)

5. **ELF Segment Permission Mapping**
   - p_flags[2] = PF_X (execute)
   - p_flags[1] = PF_W (write)
   - p_flags[0] = PF_R (read)
   - Map to PTE_AP, PTE_UXN, PTE_PXN:
     - PF_R|PF_X → PTE_AP_RO_EL0 (code, no write)
     - PF_R|PF_W → PTE_AP_RW_EL0 + PTE_UXN (data, no execute)
     - PF_R → PTE_AP_RO_EL0 + PTE_UXN (rodata)
     - Not in ELF = no map

6. **Trap Frame Rewrite on SYS_EXEC Success**
   - Modify trap frame IN-PLACE before syscall_dispatch returns
   - RETURN EARLY from syscall_dispatch without writing x0
   - If written, would clobber argc with return code

7. **Old TTBR0 Used for Path/argv Validation**
   - sys_exec must validate arg_path, arg_argv pointers while OLD TTBR0 active
   - path pointer points into old user image; becomes invalid after TTBR0 swap
   - argv captured to kernel buffer (arg_kbuf) early, before swap

8. **PHYS_TO_VIRT Only Works for Kernel-Mapped Ranges**
   - Stack built via PHYS_TO_VIRT(stack_phys) — uses kernel's TTBR1 mapping
   - This is correct because stack pages are physical; kernel has them mapped at KERNEL_VA_OFFSET + pa
   - User stack VA is independent: [USER_STACK_TOP - 16K, USER_STACK_TOP)

## Testing Checklist

1. **hello.S assembly program** — single SVC #0 exits cleanly
2. **cat.c C program with crt0** — reads file, prints bytes, exits
3. **counter.c with .data + .bss** — verifies writable segments loaded correctly
4. **argv passing** — cat file1 file2 receives argc=3, argv correct pointers
5. **stack alignment** — SP_EL0 16-byte aligned on entry; cat doesn't fault
6. **user pointer validation** — passing invalid ptr to sys_read/-write rejected
7. **EXEC_MAX_BYTES limit** — binary > 1 MiB rejected
8. **argv count limit** — argc > 32 rejected
9. **argv string bytes limit** — sum of argv strings > 1024 rejected
10. **Multiple exec calls** — exec program2 from program1; program1 stack freed
11. **ASID cleanup** — no stale TLB entries from old ASID after SYS_EXEC

## References & External Dependencies

- **kernel::exception** — trap_frame_t, syscall entry point
- **kernel::sched** — sched_current(), sched_asid_alloc(), sched_fork(), sched_kill_task()
- **kernel::mm::mmu** — USER_TEXT_BASE, USER_STACK_TOP, mmu_create_user_tables(), mmu_map_user_range(), ttbr_pack()
- **kernel::mm::pmm** — pmm_allocate_pages(), pmm_free_pages()
- **kernel::mm::heap** — kmalloc(), kfree()
- **kernel::vfs** — fd_open(), fd_read(), fd_write(), fd_close(), fd_seek()
- **kernel::elf** — ELF64 header walking (PT_LOAD iteration)
- **kernel::timer** — timer_uptime_ms()
- **kernel::net** — net_send_ping(), net_rx_poll()
- **kernel::uart** — uart_printf() (diagnostics only)
- **kernel::balloon** — balloon_inflate(), balloon_deflate(), balloon_get_status()

