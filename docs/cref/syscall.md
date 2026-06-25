# Syscall Subsystem — Porting Specification

**Subsystem:** `syscall` — Dispatch of supervisor-call (SVC) exceptions, trap frame management, ELF static loader for user executables, and POSIX syscall routing to VFS / scheduler / device drivers.

**Status:** Pure C implementation to be ported to pure Rust + inline assembly.

---

## 1. Overview

The syscall subsystem handles:
1. **SVC Exception Dispatch** — Trap frame extraction, syscall number routing, and return value serialization
2. **User Pointer Validation** — Range checks to prevent kernel pointer injection attacks
3. **ELF64 Static Binary Loader** — Parse aarch64 ET_EXEC binaries, allocate PMM pages per PT_LOAD, map with appropriate permissions, and synchronize I-cache
4. **SYS_EXEC Implementation** — Binary replacement, argv capture, stack initialization, TTBR0 swap, and reaping of old allocations
5. **Syscall Table** — 15 POSIX-like syscalls: read, write, open, close, exit, yield, sleep, getpid, lseek, uptime, net_ping, kill, fork, exec, balloon

**Entry Point:** `syscall_dispatch(trap_frame_t *frame)` — Called from the SVC exception handler in the exception vector.

**Key Constraint:** The trap frame layout must exactly match the C version (31 x-registers + elr + spsr + esr + far = 280 bytes for the struct, plus sp_el0 at offset 280 in the 288-byte on-stack layout).

---

## 2. Public API

### 2.1 Main Dispatcher

```c
void syscall_dispatch(trap_frame_t *frame);
```

**Behavior:**
- Extracts `x8` (syscall number) and `x0–x2` (first 3 args; more via context)
- Enables IRQs (sets DAIF[bit 1] = 0) to allow preemption during long syscalls
- Routes to the appropriate syscall handler based on `x8`
- Writes the return value to `frame->regs[0]` (except SYS_EXEC on success)
- Returns normally; eret epilogue restores registers and returns to user space

**Syscall Convention (ARM AAPCS64):**
- `x8` = syscall number (u64)
- `x0–x7` = arguments (7 u64 args max)
- `x0` = return value (written on eret)
- All other registers preserved across syscall

---

## 3. Trap Frame Layout

**Structure: `trap_frame_t` (280 bytes in the Rust struct)**

```c
typedef struct trap_frame {
  uint64_t regs[31];  /* x0–x30; offset 0 */
  uint64_t elr;       /* Exception Link Register; offset 248 */
  uint64_t spsr;      /* Saved Processor State Register; offset 256 */
  uint64_t esr;       /* Exception Syndrome Register; offset 264 */
  uint64_t far;       /* Fault Address Register; offset 272 */
} trap_frame_t;
```

**On-Stack Layout (288 bytes in vector.S):**
The trap frame is followed immediately by `sp_el0` at offset 280 (in the 688-byte total on-stack frame). When `sys_exec` needs to set the new user stack pointer, it accesses `frame_raw[35]` which is `sp_el0`.

**Key Offsets (for inline assembly access):**
- `regs[0]` (x0): offset 0
- `regs[8]` (x8): offset 64
- `regs[30]` (x30/lr): offset 240
- `elr`: offset 248
- `spsr`: offset 256
- `sp_el0`: offset 280 (not in struct; accessed via raw pointer arithmetic)

---

## 4. Syscall Table

### 4.1 SYS_READ (0)

```c
svc #0  /* x8=0, x0=fd, x1=buf, x2=count */
```

**Behavior:**
- Validates `buf` is inside user range `[0, USER_STACK_TOP)` for `count` bytes
- Calls `fd_read(fds, fd, buf, count)`
- Returns bytes read, or -1 on error

**Validation:** `user_buf_ok(x1, x2)`

---

### 4.2 SYS_WRITE (1)

```c
svc #0  /* x8=1, x0=fd, x1=buf, x2=count */
```

**Behavior:**
- Validates `buf` is inside user range for `count` bytes
- Calls `fd_write(fds, fd, buf, count)`
- Returns bytes written, or -1 on error

---

### 4.3 SYS_OPEN (2)

```c
svc #0  /* x8=2, x0=path (cstr ptr) */
```

**Behavior:**
- Validates `path` is a NUL-terminated string within user range and < 4096 bytes
- Calls `fd_open(fds, path)`
- Returns fd ≥ 0, or -1 on error

**Validation:** `user_str_ok(x0) >= 0`

---

### 4.4 SYS_CLOSE (3)

```c
svc #0  /* x8=3, x0=fd */
```

**Behavior:**
- Calls `fd_close(fds, fd)`
- Returns 0, or -1 on error

---

### 4.5 SYS_EXIT (4)

```c
svc #0  /* x8=4 */
```

**Behavior:**
- Calls `task_exit()` — marks the current task DEAD, relinks run queue, schedules next task
- Does not return

---

### 4.6 SYS_YIELD (5)

```c
svc #0  /* x8=5 */
```

**Behavior:**
- Calls `schedule()` — may preempt to another ready task
- Returns 0

---

### 4.7 SYS_SLEEP (6)

```c
svc #0  /* x8=6, x0=milliseconds */
```

**Behavior:**
- Calls `sleep_ms(x0)`
- Blocks current task until timer expiry
- Returns 0

---

### 4.8 SYS_GETPID (7)

```c
svc #0  /* x8=7 */
```

**Behavior:**
- Returns `sched_current()->pid` (u64)

---

### 4.9 SYS_LSEEK (8)

```c
svc #0  /* x8=8, x0=fd, x1=offset (i64), x2=whence */
```

**Behavior:**
- `whence` is one of: SEEK_SET (0), SEEK_CUR (1), SEEK_END (2)
- Calls `fd_seek(fds, fd, offset, whence)`
- Returns new offset, or -1 on error

---

### 4.10 SYS_UPTIME (9)

```c
svc #0  /* x8=9 */
```

**Behavior:**
- Returns `timer_uptime_ms()` — milliseconds since boot

---

### 4.11 SYS_NET_PING (10)

```c
svc #0  /* x8=10, x0=seq (u16) */
```

**Behavior:**
- Sends ICMP echo request with sequence `x0` to slirp gateway
- Busy-polls `net_rx_poll()` for up to ~2M spins (~200 ms on modern CPU)
- Matches reply by checking:
  - Frame size ≥ 14+20+8 bytes (Ethernet + IP + ICMP)
  - Byte[12:13] = 0x0800 (IPv4)
  - IP protocol = 1 (ICMP)
  - ICMP type = 0 (echo reply)
  - ICMP seq matches request
- Returns reply IPv4 TTL (byte[8] of IP header), or -1 on timeout

---

### 4.12 SYS_KILL (11)

```c
svc #0  /* x8=11, x0=pid */
```

**Behavior:**
- Calls `sched_kill_task(x0)`
- Returns 0 on success, -1 on failure (invalid pid, already dead, or pid==0 idle)

---

### 4.13 SYS_FORK (12)

```c
svc #0  /* x8=12 */
```

**Behavior:**
- Calls `sched_fork(current_task, frame)`
- Duplicates current task's user address space (deep copy of TTBR0 page tables and data)
- Parent receives child pid (> 0)
- Child, when first scheduled, returns 0 via special `fork_return` entrypoint
- Returns child pid, or -1 on error (alloc failure, pid exhaustion)

---

### 4.14 SYS_EXEC (13)

```c
svc #0  /* x8=13, x0=path (cstr), x1=argv (array of cstr pointers or NULL) */
```

**Behavior:** See section 5 (SYS_EXEC Details).

**Return Value:**
- On success: **does not return**; trap frame is rewritten so eret lands in the new binary's entry point with clean registers
- On failure: returns -1 to user via normal x0 writeback

---

### 4.15 SYS_BALLOON (14)

```c
svc #0  /* x8=14, x0=op, x1=page_count (for inflate/deflate) */
```

**Op Codes:**
- `BALLOON_OP_INFLATE` (0): Hand `x1` pages to host. Returns pages actually inflated.
- `BALLOON_OP_DEFLATE` (1): Reclaim `x1` pages from host. Returns pages actually deflated.
- `BALLOON_OP_ACTUAL` (2): Query current balloon size. Ignores `x1`. Returns page count.
- `BALLOON_OP_TARGET` (3): Query host's target size. Ignores `x1`. Returns page count.

---

## 5. SYS_EXEC Details

### 5.1 Overview

Replaces the calling task's user image (text, data, stack) with a new ELF64 aarch64 ET_EXEC binary. The new binary begins execution at its entry point with a clean register state and a fresh user stack populated with argc/argv.

### 5.2 Constants

```c
#define EXEC_MAX_BYTES        (1U << 20)  /* 1 MiB cap on binary size */
#define EXEC_MAX_ARGC         32          /* max argv count */
#define EXEC_ARG_BYTES_MAX    1024        /* total string-byte budget for argv */
#define USER_PATH_MAX         4096        /* max path length (matches POSIX) */
```

### 5.3 Memory Model

**Before exec:**
- User TTBR0 points to task's old page tables
- Old text, data, and stack are mapped therein

**After exec (on success):**
- User TTBR0 points to a fresh page table
- Text regions (PT_LOAD with PF_X): RO + EL0-executable, mapped at p_vaddr
- Data+BSS regions (PT_LOAD with PF_W): RW + UXN, mapped at p_vaddr
- Read-only data (rare): RO + UXN
- User stack: Fresh 16 KiB (4 pages), RW + UXN, mapped at `[USER_STACK_TOP - 16KB, USER_STACK_TOP)`
- Entry registers:
  - `x0` = argc (int32_t promoted to u64)
  - `x1` = argv (user VA of the argv array)
  - `x2` = 0 (envp, not used)
  - `x3–x30` = 0
  - `sp_el0` = user stack pointer (16-byte aligned, below argv array)
  - `pc` (elr) = binary entry point (e_entry)

### 5.4 Algorithm

**Phase 1: Validate Arguments (old TTBR0 active)**

1. Validate `path` is a NUL-terminated user string ≤ 4096 bytes
2. If `argv != 0`, walk the NULL-terminated array:
   - For each argv[i]:
     - Validate argv slot is inside user range
     - Validate argv[i] (pointer) is not NULL
     - Validate argv[i] points to a NUL-terminated string ≤ 4096 bytes
     - Copy string to kernel buffer `arg_kbuf[EXEC_ARG_BYTES_MAX]`
     - Record offset in `arg_offsets[]`
   - Reject if argc ≥ EXEC_MAX_ARGC or total bytes ≥ EXEC_ARG_BYTES_MAX

**Phase 2: Open and Read Binary**

3. Call `fd_open(fds, path)` → fd
4. Call `fd_seek(fds, fd, 0, SEEK_END)` → size
5. Reject if size ≤ 0 or size > EXEC_MAX_BYTES
6. Allocate kernel buffer: `kmalloc(size)`
7. Call `fd_read(fds, fd, kbuf, size)` → got
8. Reject if got ≠ size
9. Call `fd_close(fds, fd)`

**Phase 3: Allocate New Address Space**

10. Allocate fresh user stack: `pmm_allocate_pages(USER_STACK_PAGES)` → stack_phys
11. Zero the stack: `memset(PHYS_TO_VIRT(stack_phys), 0, ...)`
12. Create fresh user page tables: `mmu_create_user_tables()` → new_l0
13. Call `elf_load(kbuf, size, new_l0, &img)` to parse and load PT_LOAD segments
    - On elf_load failure, free stack_phys and new_l0, then return -1

**Phase 4: Map User Stack into New Address Space**

14. Map the stack with: `mmu_map_user_range(new_l0, USER_STACK_TOP - USER_STACK_PAGES*4KB, stack_phys, USER_STACK_PAGES, stack_flags)`
    - `stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN`

**Phase 5: Swap TTBR0 and Save Old References**

15. Save old state: `old_ttbr0 = cur->ttbr0`, `old_ustack_phys = cur->ustack_phys`, `old_image = cur->exec_image`
16. Allocate fresh ASID: `new_asid = sched_asid_alloc()` (ensures new TLB entries never alias old)
17. Pack new TTBR0: `new_ttbr0 = ttbr_pack(new_l0, new_asid)`
18. Update task state:
    - `cur->ttbr0 = new_ttbr0`
    - `cur->ustack_phys = stack_phys`
    - `cur->user_sp = USER_STACK_TOP`
    - `cur->exec_image = img`
19. Swap TTBR0: `msr ttbr0_el1, new_ttbr0; isb`

**Phase 6: Rewrite Trap Frame**

20. Zero x0–x30: `frame->regs[0..30] = 0`
21. Set `frame->elr = img.entry`
22. Set `frame->spsr = 0` (EL0t, IRQs unmasked)

**Phase 7: Build argv on New Stack (if argc > 0)**

23. Strings are stored at high end of stack, growing downward
24. `strings_user_base = USER_STACK_TOP - arg_bytes`
25. Copy `arg_kbuf` into stack via kernel mapping: `PHYS_TO_VIRT(stack_phys) + (strings_user_base - stack_user_lo)`
26. argv array of pointers is 16-byte aligned, below the strings
27. `argv_user_top = strings_user_base & ~0xF`
28. `argv_user_base = argv_user_top - (argc+1)*8`
29. Write argv[0..argc-1] as user VAs of strings; argv[argc] = 0
30. `new_sp = argv_user_base & ~0xF` (16-byte aligned)
31. Set `frame->regs[0] = argc`, `frame->regs[1] = argv_user_base`, `frame->regs[2] = 0`
32. Write `frame_raw[35] = new_sp` (sets sp_el0)

**Phase 8: Reap Old Allocations**

33. Free old stack: `pmm_free_pages(old_ustack_phys, USER_STACK_PAGES)` (if non-zero)
34. Free old per-segment regions: Loop through `old_image.regions[]` and call `pmm_free_pages()`
35. Invalidate old TLB entries: `tlbi aside1, asid << 48; dsb ish; isb`
36. Free old page tables: `mmu_free_user_tables(old_ttbr0 physical address)`
37. Free kernel buffer: `kfree(kbuf)`

**On Success:** Return normally; eret epilogue uses the rewritten frame.

**On Failure:** Return -1; normal syscall epilogue copies -1 into x0 and eret to original SVC site.

---

## 6. ELF Loader (elf_load)

### 6.1 Function Signature

```c
int elf_load(const uint8_t *kbuf, size_t size, uint64_t *user_l0,
             elf_image_t *out);
```

**Returns:** 0 on success, -1 on failure (any PMM allocations made before failure are freed before return).

### 6.2 ELF Header Constants

```c
#define ELF_MAGIC0     0x7F
#define ELF_MAGIC1     'E'
#define ELF_MAGIC2     'L'
#define ELF_MAGIC3     'F'

#define ELFCLASS64     2       /* 64-bit */
#define ELFDATA2LSB    1       /* little-endian */
#define EV_CURRENT     1       /* version 1 */

#define ET_EXEC        2       /* executable (not PIE/shared) */
#define EM_AARCH64     0xB7   /* ARM AArch64 */
```

### 6.3 Program Header Constants

```c
#define PT_NULL        0
#define PT_LOAD        1       /* loadable segment */
#define PT_PHDR        6

#define PF_X           (1u << 0)  /* executable */
#define PF_W           (1u << 1)  /* writable */
#define PF_R           (1u << 2)  /* readable */
```

### 6.4 Elf64_Ehdr Structure (52 bytes, packed)

```c
typedef struct __attribute__((packed)) {
  unsigned char e_ident[16];     /* [0..15] */
  uint16_t      e_type;          /* [16..17] */
  uint16_t      e_machine;       /* [18..19] */
  uint32_t      e_version;       /* [20..23] */
  uint64_t      e_entry;         /* [24..31] */
  uint64_t      e_phoff;         /* [32..39] program header offset */
  uint64_t      e_shoff;         /* [40..47] section header offset */
  uint32_t      e_flags;         /* [48..51] */
  uint16_t      e_ehsize;        /* [52..53] ELF header size */
  uint16_t      e_phentsize;     /* [54..55] program header entry size */
  uint16_t      e_phnum;         /* [56..57] number of program headers */
  uint16_t      e_shentsize;     /* [58..59] section header entry size */
  uint16_t      e_shnum;         /* [60..61] number of section headers */
  uint16_t      e_shstrndx;      /* [62..63] section header string table index */
} Elf64_Ehdr;
```

### 6.5 Elf64_Phdr Structure (56 bytes, packed)

```c
typedef struct __attribute__((packed)) {
  uint32_t p_type;      /* [0..3] segment type (PT_LOAD, etc.) */
  uint32_t p_flags;     /* [4..7] segment flags (PF_R, PF_W, PF_X) */
  uint64_t p_offset;    /* [8..15] file offset */
  uint64_t p_vaddr;     /* [16..23] virtual address */
  uint64_t p_paddr;     /* [24..31] physical address (ignored) */
  uint64_t p_filesz;    /* [32..39] bytes in file */
  uint64_t p_memsz;     /* [40..47] bytes in memory (includes .bss) */
  uint64_t p_align;     /* [48..55] alignment requirement */
} Elf64_Phdr;
```

### 6.6 Output: elf_image_t

```c
#define ELF_MAX_REGIONS 4

typedef struct {
  uintptr_t phys;      /* PMM-allocated physical base for this region */
  uint64_t  pages;     /* number of pages allocated */
} elf_region_t;

typedef struct {
  uint64_t      entry;           /* virtual address of program entry */
  int           region_count;    /* number of valid entries in regions[] */
  elf_region_t  regions[ELF_MAX_REGIONS];  /* one per PT_LOAD */
} elf_image_t;
```

### 6.7 Validation Steps

1. **File size:** Must be at least 52 bytes (sizeof Elf64_Ehdr)
2. **Magic:** e_ident[0..3] must be `0x7F 'E' 'L' 'F'`
3. **Class:** e_ident[4] == ELFCLASS64
4. **Data:** e_ident[5] == ELFDATA2LSB
5. **Version:** e_ident[6] == EV_CURRENT
6. **Type:** e_type == ET_EXEC (reject PIE/shared)
7. **Machine:** e_machine == EM_AARCH64
8. **Phdr Entry Size:** e_phentsize == 56 (sizeof Elf64_Phdr)
9. **Phdr Count:** 0 < e_phnum ≤ 32
10. **Phdr Table:** e_phoff + e_phnum * 56 ≤ file size
11. **Per-segment:** For each PT_LOAD:
    - p_filesz ≤ p_memsz
    - [p_offset, p_offset+p_filesz) inside file
    - [p_vaddr, p_vaddr+p_memsz) inside user range [0, USER_STACK_TOP)
    - region_count < ELF_MAX_REGIONS
    - p_flags must be one of: R (ro), R|X (ro+x), R|W (rw) — reject R|W|X or no-R

### 6.8 Loading Algorithm

For each PT_LOAD segment:

1. **Permission Translation:**
   - R|X → `PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_PXN` (RO + EL0-X + kernel no-X)
   - R|W → `PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN` (RW + EL0 no-X + kernel no-X)
   - R → `PTE_ATTRIDX(1) | PTE_AP_RO_EL0 | PTE_UXN | PTE_PXN`

2. **Page Allocation:**
   - `va_lo = p_vaddr & ~(PAGE_SIZE-1)` (align down to 4KB)
   - `va_hi = (p_vaddr + p_memsz + PAGE_SIZE-1) & ~(PAGE_SIZE-1)` (align up)
   - `pages = (va_hi - va_lo) / PAGE_SIZE`
   - Allocate: `phys = pmm_allocate_pages(pages)`

3. **Copy & Zero:**
   - Zero entire allocation: `memset(PHYS_TO_VIRT(phys), 0, pages*4KB)`
   - Copy file bytes: `intra = p_vaddr - va_lo; memcpy(PHYS_TO_VIRT(phys) + intra, kbuf + p_offset, p_filesz)`
   - Leftover bytes (BSS) remain zeroed

4. **Icache Sync (executable segments only):**
   - Read `CTR_EL0` to extract I-line and D-line sizes
   - `i_line = 4 << (CTR_EL0[3:0])`
   - `d_line = 4 << (CTR_EL0[19:16])`
   - For each D-line in the range: `dc cvau, va`
   - Barrier: `dsb ish`
   - For each I-line in the range: `ic ivau, va`
   - Barriers: `dsb ish; isb`

5. **Map into user page table:**
   - `mmu_map_user_range(user_l0, va_lo, phys, pages, pte_flags)`

6. **Record region:** Store phys, pages in `out->regions[out->region_count++]`

### 6.9 Entry Point Validation

After all segments are loaded, verify that `e_entry` lies inside an executable PT_LOAD:

```c
for each PT_LOAD:
  if PT_LOAD.p_flags & PF_X
    and e_entry >= p_vaddr
    and e_entry < p_vaddr + p_memsz:
    entry_ok = 1
    break
if !entry_ok: return -1
```

---

## 7. User Pointer Validation

### 7.1 Buffer Validation

```c
static inline int user_buf_ok(uint64_t ptr, size_t len);
```

**Behavior:**
- Return 1 if `[ptr, ptr+len)` is entirely inside `[0, USER_STACK_TOP)`
- Return 0 otherwise
- Special case: len == 0 returns 1 (allows zero-length buffers)
- Overflow guard: if `ptr + len < ptr` (wraps), return 0

**Implementation:**
```
if len == 0: return 1
if ptr + len < ptr: return 0 (overflow)
if ptr + len > USER_STACK_TOP: return 0
return 1
```

### 7.2 String Validation

```c
static inline int64_t user_str_ok(uint64_t ptr);
```

**Behavior:**
- Scan for NUL terminator starting at `ptr`
- Return string length (excluding NUL) on success
- Return -1 if:
  - `ptr >= USER_STACK_TOP` (already outside user range)
  - No NUL found within min(USER_PATH_MAX, USER_STACK_TOP - ptr) bytes

**USER_PATH_MAX:** 4096 bytes

---

## 8. User Address Space Layout

**Kernel:** Via TTBR1 (higher-half kernel)
- Kernel VA offset: `0xFFFF000000000000`
- Kernel code, data, heap, stacks mapped at offset + physical address

**User (TTBR0, per-task):**
- Code: `[0x00400000, ...)` User code (loaded by ELF parser)
- Stack: `[USER_STACK_TOP - 16KB, USER_STACK_TOP)` = `[0x007FC000, 0x00800000)`
- User range limit: `USER_STACK_TOP = 0x00800000` (8 MB)

**Unused:**
- `[0, USER_TEXT_BASE)` and `(stack_top, 0xFFFF000000000000)` are unmapped or kernel

**Constants:**
```c
#define USER_TEXT_BASE   0x00400000ULL
#define USER_STACK_TOP   0x00800000ULL
#define USER_STACK_PAGES 4
#define PAGE_SIZE        4096
```

---

## 9. Dependencies

### 9.1 Subsystems Called

- **sched:** `sched_current()`, `sched_asid_alloc()`, `sched_fork()`, `sched_kill_task()`, `schedule()`, `task_exit()`
- **mmu:** `mmu_create_user_tables()`, `mmu_map_user_range()`, `mmu_free_user_tables()`, `ttbr_pack()`, `ttbr_asid()`, `ttbr_baddr()`
- **pmm:** `pmm_allocate_pages()`, `pmm_free_pages()`
- **heap:** `kmalloc()`, `kfree()`
- **vfs:** `fd_open()`, `fd_read()`, `fd_write()`, `fd_close()`, `fd_seek()` + fd_table_t
- **timer:** `timer_uptime_ms()`, `sleep_ms()`
- **uart:** `uart_printf()`, `uart_errorln()`
- **net:** `net_send_ping()`, `net_rx_poll()`
- **balloon:** `balloon_inflate()`, `balloon_deflate()`, `balloon_get_status()`
- **exception:** `trap_frame_t`, `fork_return()` entrypoint

### 9.2 Subsystems That Call This

- **exception:** SVC handler (EC_SVC_AARCH64) calls `syscall_dispatch(frame)`
- **kernel/init:** May call `sys_*` functions inline for early setup

---

## 10. Key Gotchas & Correctness Issues

### 10.1 TTBR0 Swap Order

In `sys_exec`, the trap frame is rewritten *after* TTBR0 is swapped. This is safe because:
- New TTBR0 has a fresh ASID, so TLB entries are isolated
- Trap frame lives on the kernel stack (TTBR1, not affected by TTBR0)
- When eret restores sp_el0 and returns, the new address space is already active

### 10.2 sp_el0 Is Not in trap_frame_t

The C struct is 280 bytes; sp_el0 lives at offset 280 in the 288-byte on-stack layout. When setting sp_el0 in Rust, access it via:
- Raw pointer arithmetic: `frame_raw[35]` (if frame_raw is `*mut u64`)
- Or embed sp_el0 in a larger wrapper struct

### 10.3 ASID Allocation and Wraparound

`sched_asid_alloc()` cycles through ASIDs 1–65535 and wraps to 1 after 65535 uses. On wraparound, it does a global TLB flush to ensure recycled ASIDs don't alias stale TLB entries. Rust must preserve this behavior.

### 10.4 Icache Coherence

After `memcpy` ELF code into PMM pages via the kernel TTBR1 mapping, code must not execute until icache is synced. The sequence (DC CVAU, DSB, IC IVAU, DSB, ISB) is architecture-required and must not be elided, even on QEMU.

### 10.5 Zero-Length Buffers

`user_buf_ok(ptr, 0)` returns 1 regardless of `ptr`. This matches POSIX (sys_read/write with count=0). Be careful not to null-check ptr in Rust—it's valid.

### 10.6 argv Capture Happens Before TTBR0 Swap

When `sys_exec` reads argv, the user TTBR0 is still active. After the swap, the old TTBR0 (and all its mappings) are freed. Rust implementation must capture argv into kernel buffers *before* the swap, not after.

### 10.7 Preemption During Syscall

`syscall_dispatch` explicitly enables IRQs (DAIF[1]=0) to allow preemption during long syscalls (e.g., reading from UART). Rust must do the same via inline asm:
```
msr daifclr, #2  // clear bit 1 (IRQ mask)
```

### 10.8 fork_return Entry Point

After `sched_fork()`, the child task needs a special return path (`fork_return` asm stub) that calls `eret` without going through the full exception epilogue. The trap frame is pre-configured so x0 contains 0. Rust must expose this same function or equivalent.

### 10.9 Bad User Pointers Still Fault the Kernel

`user_buf_ok` only checks the virtual address range, not whether pages are mapped. A user pointer inside the range can point to an unmapped page, causing a data abort in the kernel. The C code assumes this is acceptable (non-fatal fault handling is out of scope); Rust must preserve this.

---

## 11. Hardware Constants & Register Access

### 11.1 DAIF Register

**Set IRQ unmask (enable):**
```asm
msr daifclr, #2  // clear bit 1 (IRQ mask); daif[1]=0 means IRQs enabled
```

### 11.2 TTBR0_EL1

**Read and write:**
```asm
mrs ttbr0, ttbr0_el1
msr ttbr0_el1, <reg>
isb
```

**Layout:**
- [63:48] ASID
- [47:1] page table base address (page-aligned, bits [11:0]=0)
- [0] CnP (context number padding, we leave 0)

### 11.3 TLB Invalidation

**Invalidate user entries (ASID 1–65535):**
```asm
tlbi aside1, %0  // arg = asid << 48
dsb ish
isb
```

**Invalidate all:**
```asm
tlbi vmalle1
dsb ish
isb
```

### 11.4 CTR_EL0 (Cache Type Register)

**Read:**
```asm
mrs ctr, ctr_el0
```

**Field extraction:**
- `[3:0]` DminLine = log2(words per D-line); words = 4 bytes, so d_line = 4 << bits[3:0]
- `[19:16]` IminLine = log2(words per I-line); i_line = 4 << bits[19:16]

### 11.5 Cache Maintenance Instructions

**DC CVAU:** Clean data cache line to point of unification (user VA)
```asm
dc cvau, %0  // arg = user VA
```

**IC IVAU:** Invalidate instruction cache line to point of unification (user VA)
```asm
ic ivau, %0  // arg = user VA
```

---

## 12. Rust Module Structure & Design

### 12.1 Module Organization

```
syscall/
├── mod.rs          — dispatcher, syscall routing
├── exec.rs         — sys_exec implementation
├── elf_load.rs     — ELF parser and loader
├── validate.rs     — user pointer validation
└── constants.rs    — all #define constants + struct layouts
```

### 12.2 Core Structures

```rust
pub struct TrapFrame {
    regs: [u64; 31],        // x0–x30
    elr: u64,              // exception link register
    spsr: u64,             // saved processor state
    esr: u64,              // exception syndrome
    far: u64,              // fault address
}
// Note: sp_el0 at +280 bytes (accessed via raw pointer)

pub struct ElfImage {
    entry: u64,
    region_count: usize,
    regions: [ElfRegion; ELF_MAX_REGIONS],
}

pub struct ElfRegion {
    phys: usize,   // physical address (from PMM)
    pages: u64,    // page count
}

pub const USER_STACK_TOP: u64 = 0x00800000;
pub const USER_TEXT_BASE: u64 = 0x00400000;
pub const USER_STACK_PAGES: u64 = 4;
pub const PAGE_SIZE: u64 = 4096;
```

### 12.3 Locking Strategy

- **Syscall dispatch:** Called from exception handler, no preemption at entry; enables IRQs midway (re-entrant)
- **Task state:** Protected by disabling IRQs during sched_current() / ttbr swap in exec
- **PMM:** Uses internal spinlock (same as C)
- **VFS:** Uses internal spinlock per fd_table (same as C)

### 12.4 No_std / alloc Usage

- Syscall subsystem is **no_std** (no heap allocation except kmalloc/kfree)
- ELF loader uses fixed arrays, no Vec
- VFS fd_table is fixed-size (64 fds per task)

### 12.5 Inline Assembly Requirements

Must stay in **inline asm** (not pure Rust):
1. **DAIF IRQ unmask:** `msr daifclr, #2`
2. **TTBR0 read/write + ISB:** `mrs/msr ttbr0_el1; isb`
3. **TLB invalidate:** `tlbi aside1; dsb ish; isb`
4. **Cache ops:** `dc cvau; ic ivau; dsb; isb` (icache sync in elf_load)
5. **CTR_EL0 read:** `mrs ctr_el0`

**Wrapper functions recommended:**
```rust
#[inline]
fn enable_irqs() {
    unsafe { asm!("msr daifclr, #2", options(nomem, nostack)) }
}

#[inline]
fn set_ttbr0(ttbr0: u64) {
    unsafe { 
        asm!("msr ttbr0_el1, {}; isb", in(reg) ttbr0, 
             options(preserves_flags))
    }
}
```

### 12.6 Key Functions to Expose

```rust
pub fn syscall_dispatch(frame: &mut TrapFrame) -> Result<(), i64>;
pub fn elf_load(kbuf: &[u8], user_l0: &mut [u64; ???], 
                out: &mut ElfImage) -> Result<(), i64>;
pub fn user_buf_ok(ptr: u64, len: usize) -> bool;
pub fn user_str_ok(ptr: u64) -> Result<usize, i64>;  // returns string len
```

### 12.7 Error Handling

- Syscalls return `-1i64` on error (matches C convention)
- `sys_exec` returns nothing on success (frame rewritten); -1 on failure
- `elf_load` frees all PMM allocations before returning error
- VFS operations already return -1 on error (fd_table ops)

---

## 13. Testing & Verification

### 13.1 Unit Tests

- User pointer validation (boundary cases, overflow)
- ELF parsing (magic, class, entry validation)
- TTBR packing/unpacking (ASID round-trip)
- Stack layout (argv alignment)

### 13.2 Integration Tests

- Load a simple test binary via sys_exec
- Verify entry point is reached with correct argc/argv
- Fork and exec in sequence
- Read/write syscalls with boundary buffers

### 13.3 Known Limitations

- No copy_from_user exception handling (unmapped pages in user range still fault)
- No signal handlers (syscalls don't return to user on fault)
- Icache sync is only needed on real hardware; QEMU is self-coherent

---

## 14. Summary Table

| Item | Value / Description |
|------|-------------------|
| Entry Point | `syscall_dispatch(trap_frame_t *)` |
| Syscall Convention | x8 = num, x0–x7 = args, x0 = return |
| User Range | [0x0, 0x800000) |
| Stack | [0x7FC000, 0x800000) — 16 KiB, grows down |
| ELF Type | ET_EXEC, EM_AARCH64, static only |
| Max Binary Size | 1 MiB |
| Max Regions | 4 PT_LOAD segments |
| ASID Wraparound | Every 65535 allocs; global TLB flush on wrap |
| Preemption | Enabled mid-syscall (DAIF[1]=0) |
| Icache Sync | DC CVAU → DSB → IC IVAU → DSB → ISB |
| Page Size | 4 KiB |
| Page Alloc Granule | 4 KiB (same as MMU) |

---

## 15. File References

- **Source:** `/local/home/rituu/fermi-claude-rs/src/syscall/syscall.c`
- **Header:** `/local/home/rituu/fermi-claude-rs/src/syscall/syscall.h`
- **ELF Loader:** `/local/home/rituu/fermi-claude-rs/src/syscall/elf.c`
- **ELF Header:** `/local/home/rituu/fermi-claude-rs/src/syscall/elf.h`
- **Related:** mmu.h, pmm.h, sched.h, vfs.h, timer.h, exception.h

