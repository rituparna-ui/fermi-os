# Fermi OS Kernel Subsystem Specification

## Overview

The **kernel** subsystem is the top-level orchestration layer for the Fermi OS bare-metal aarch64 kernel. It coordinates the boot sequence, initializes all hardware and software subsystems, spawns the initial task set (task_a, task_b, task_shell, task_crash, and the netd daemon), and runs the idle task scheduler loop. The kernel operates at Exception Level 1 (EL1) with the Memory Management Unit (MMU) enabled, running in the upper-half virtual address space (VA >= 0xFFFF000000000000). It provides the syscall interface to EL0 user tasks and delegates subsystem coordination to lower-level modules (PMM, MMU, GIC, timer, scheduler, VFS, networking).

## Architecture Overview

- **Bootstrap (boot.S → early_init)**: Physical-address execution, BSS zeroing, FP/SIMD enable, UART init, exception setup, PMM init, MMU enable, then jump to upper-half kernel_main.
- **kernel_main**: Upper-half execution, device initialization (PCI, VirtIO, FAT32), task spawning, timer setup, idle task loop (WFI).
- **Syscall Interface**: SYS_READ, SYS_WRITE, SYS_OPEN, SYS_CLOSE, SYS_EXIT, SYS_SLEEP, SYS_FORK, SYS_EXEC, SYS_KILL, SYS_NET_PING, SYS_UPTIME, SYS_GETPID, SYS_BALLOON dispatched via exception.c EC_SVC_AARCH64.
- **Kernel Tasks**: task_a (FAT32 demo), task_b (RNG reader), task_shell (interactive user shell), task_crash (fault handler test), netd (kernel-mode background pinger).
- **User Task Shell**: task_shell implements the EL0 command interpreter with hand-written user-space syscall wrappers (sys_read, sys_write, sys_open, etc.) and parsing logic for 20+ built-in commands.

## Public API

### Early Bootstrap

```c
void early_init(void);
```
**Purpose**: Physical-address initialization before MMU enable. Called from boot.S.
**Actions**:
1. zero_bss(): Memset .bss from `__bss_start` to `__bss_end` to 0
2. enable_fp_simd(): Set CPACR_EL1.FPEN = 0b11 (bits [21:20]) and ISB to enable SIMD for varargs
3. uart_init(): Initialize UART at 0x09000000
4. uart_println("Fermi OS - Booting Up...")
5. print_current_el(): Print current exception level
6. exceptions_init(): Install EL1 exception handlers at EL0 physical addresses
7. pmm_init(MEM_START=0x40000000, MEM_SIZE=8GB): Initialize page allocator
8. pmm_print_info(): Log memory stats
9. mmu_init(): Enable MMU, return L1 page table pointer (physical)
10. mmu_run_tests(l1_phys): Validate page table walk and remapping
11. uart_println("[BOOT] MMU Enabled. Jumping to Upper Half")

**Return**: None (falls through to boot.S upper-half jump).

### Upper-Half Kernel Initialization

```c
void kernel_main(void);
```
**Purpose**: Upper-half kernel entry point. Runs after MMU enable and upper-half VA switch.
**Actions**:
1. mmio_switch_to_upper(): Relocate MMIO access to upper-half kernel VA offset
2. exceptions_init_upper(): Relocate exception vector (VBAR_EL1) to upper-half
3. pmm_relocate_upper(): Migrate PMM bitmap to upper-half TTBR1 mapping
4. uart_printf("[KERNEL] kernel_main address: %x\n", (uint64_t)(uintptr_t)kernel_main)
5. uint64_t sp; __asm__("mov %0, sp" : "=r"(sp)); uart_printf("[KERNEL] Stack Pointer: %x\n", sp)
6. cpu_init(): Initialize CPU cycle counter (PMCCNTR_EL0) and cache MIDR
7. heap_init(): Set up kernel heap (1 MB, 16-byte aligned blocks)
8. gic_init(): Configure GICv3 distributor and redistributor for IRQ dispatch
9. pci_enumerate_bus(): Walk PCI config space, initialize bridges
10. pci_virtio_rng_init(): Attach VirtIO RNG device at /dev/rng
11. pci_virtio_blk_init(): Attach VirtIO block device
12. pci_virtio_net_init(): Attach VirtIO network device (sends seq=1 ping)
13. pci_virtio_balloon_init(): Attach VirtIO balloon device
14. pci_virtio_console_init(): Attach VirtIO console (/dev/vcons)
15. fat32_mount(): Mount FAT32 from VirtIO block device
16. vfs_init(): Initialize VFS root and mount table
17. devices_register(): Create /dev/console, /dev/null, /dev/zero, /dev/rng in VFS
18. vfs_create_node(vfs_root(), "mnt", VNODE_DIR): Create /mnt directory
19. vfs_create_node(mnt, "fat32", VNODE_DIR): Create /mnt/fat32 directory
20. fat32_vfs_mount("/mnt/fat32"): Mount FAT32 into VFS
21. proc_init(): Initialize /proc pseudo-filesystem
22. sched_init(): Initialize task scheduler (idle task, run queue)
23. sched_create_task("task_a", task_a): Spawn user task A
24. sched_create_task("task_b", task_b): Spawn user task B
25. sched_create_task("task_shell", task_shell): Spawn interactive shell
26. sched_create_task("task_crash", task_crash): Spawn crash handler test
27. sched_create_kernel_task("netd", netd): Spawn EL1 background pinger
28. timer_init(): Set up ARM generic timer (CNTV_TVAL_EL0)
29. timer_start(TIMER_INTERVAL_MS=10): Program 10 ms periodic interrupt (PPI 30)
30. uart_println("[KERNEL] Ready! running idle task...")
31. Loop: __asm__("wfi") (Wait For Interrupt) forever

**Return**: Never (idle loop runs until crash).

```c
void kernel_panic_return(void);
```
**Purpose**: Called if kernel_main returns (should never happen).
**Action**: kernel_panic("kernel_main returned unexpectedly")

### Syscall Wrappers (Inline in kernel.c for User-Space Use)

These are defined as inline assembly wrappers for user-space syscall invocation. They use the `svc #0` instruction to trap into EL1 via EC_SVC_AARCH64.

```c
int64_t sys_read(int fd, void *buf, uint64_t count);
```
- **x0**: fd (input), returns bytes read (output)
- **x1**: buf
- **x2**: count
- **x8**: 0 (SYS_READ)
- **Effect**: svc #0 traps to syscall_dispatch

```c
int64_t sys_write(int fd, const char *buf, uint64_t len);
```
- **x0**: fd (input), returns bytes written (output)
- **x1**: buf
- **x2**: len
- **x8**: 1 (SYS_WRITE)

```c
int64_t sys_open(const char *path);
```
- **x0**: path (input/output: FD or error)
- **x8**: 2 (SYS_OPEN)

```c
int64_t sys_close(int fd);
```
- **x0**: fd (input/output: 0 or error)
- **x8**: 3 (SYS_CLOSE)

```c
void sys_exit(void);
```
- **x8**: 4 (SYS_EXIT)
- **Effect**: Noreturn; calls task_exit() in kernel

```c
void sys_sleep(uint64_t ms);
```
- **x0**: ms
- **x8**: 6 (SYS_SLEEP)

```c
int64_t sys_getpid(void);
```
- **x0**: (output) current task PID
- **x8**: 7 (SYS_GETPID)

```c
int64_t sys_uptime(void);
```
- **x0**: (output) ms since boot
- **x8**: 9 (SYS_UPTIME)

```c
int64_t sys_net_ping(uint16_t seq);
```
- **x0**: seq (input), returns TTL (output) or -1 on timeout
- **x8**: 10 (SYS_NET_PING)

```c
int64_t sys_kill(int pid);
```
- **x0**: pid (input), returns 0 or -1
- **x8**: 11 (SYS_KILL)

```c
int64_t sys_fork(void);
```
- **x0**: (output) child PID for parent, 0 for child, <0 on error
- **x8**: 12 (SYS_FORK)

```c
int64_t sys_exec(const char *path, const char *const *argv);
```
- **x0**: path (input/output: 0 or error)
- **x1**: argv (NULL-terminated array of pointers)
- **x8**: 13 (SYS_EXEC)

```c
int64_t sys_balloon(uint64_t op, uint64_t n);
```
- **x0**: op (input): BALLOON_OP_INFLATE (0), DEFLATE (1), ACTUAL (2), TARGET (3)
- **x1**: n (page count, input)
- **x8**: 14 (SYS_BALLOON)
- **Returns**: Page count or target (for op 2/3), or count processed (for op 0/1)

### User-Space Shell Implementation (task_shell)

```c
static void task_shell(void);
```
**Purpose**: Interactive EL0 command interpreter running in user-space.
**Operation**:
1. Prints welcome banner
2. Loop: reads line from stdin via u_read_line, parses command, dispatches handler
3. Implements 20+ built-in commands via string matching:
   - **help**: Print command list
   - **pid**: Print my task PID
   - **uptime**: Print ms since boot
   - **ps**: cat /proc/tasks
   - **free**: cat /proc/meminfo
   - **ifconfig**: cat /proc/netinfo
   - **irqs**: cat /proc/interrupts
   - **version**: cat /proc/version
   - **cpuinfo**: cat /proc/cpuinfo
   - **stack**: Stress demand-paged stack growth (allocate 64 KiB local, touch every page)
   - **cat <path>**: Print file contents
   - **hexdump <path>**: Hex+ASCII dump
   - **echo <text>**: Print text
   - **kill <pid>**: Terminate task
   - **fork**: Spawn child, both parent and child print
   - **exec <path> [args...]**: Replace with flat binary from disk
   - **balloon [status|inflate N|deflate N]**: VirtIO balloon control
   - **vlog <text>**: Send to /dev/vcons (virtio-console host log)
   - **top**: 5x refresh of /proc/tasks, /proc/meminfo, /proc/netinfo (1 s intervals)
   - **ping**: One-shot ICMP echo to slirp gateway (10.0.2.2)
   - **sleep <ms>**: Block for milliseconds
   - **clear**: ANSI clear screen
   - **reboot**: PSCI SYSTEM_RESET (HVC #0, x0=0x84000009)
   - **exit**: Terminate shell

**User-Space Helpers** (inline utility functions for task_shell):
- `u_render_uint(buf, max, v)`: Render uint64_t to decimal string
- `u_streq(a, b)`: String equality
- `u_starts_with(s, prefix)`: Prefix check
- `u_read_line(buf, max)`: Read line with echo and backspace support
- `sh_print(s)`: Write string to stdout
- `u_atou(s)`: Parse unsigned decimal
- `sh_help()`: Print help banner
- `sh_pid()`: Print PID
- `sh_uptime()`: Print uptime
- `sh_cat(path)`: Print file

### Kernel-Mode Tasks

```c
static void task_a(void);
```
**Purpose**: Demo EL0 user task. Reads /mnt/fat32/HELLO.TXT, prints via stdout, demos SYS_GETPID, queries /proc/netinfo and /proc/interrupts.
**Actions**:
1. Print "[Task A] reading /mnt/fat32/HELLO.TXT\n"
2. Call sys_getpid(), render "[Task A] pid=N\n", print
3. sys_open("/mnt/fat32/HELLO.TXT"), sys_read into 256-byte buffer, sys_write, sys_close
4. Print "[Task A] cat /proc/netinfo\n"
5. sys_open("/proc/netinfo"), sys_read, sys_write, sys_close
6. Print "[Task A] cat /proc/interrupts\n"
7. sys_open("/proc/interrupts"), sys_read, sys_write, sys_close
8. Print "[Task A] done\n"
9. sys_exit()

```c
static void task_b(void);
```
**Purpose**: Demo EL0 user task. Reads 4 random bytes from /dev/rng every 500 ms, renders as hex, prints.
**Actions**:
1. Call sys_getpid(), print "[Task B] pid=N\n"
2. sys_open("/dev/rng")
3. Loop forever:
   - sys_read(fd, r, 4) -> read 4 bytes
   - Render as "[Task B] rng: HH HH HH HH\n" (hex)
   - sys_write(1, ...)
   - sys_sleep(500)

```c
static void task_crash(void);
```
**Purpose**: Intentional fault handler test. Dereferences unmapped user VA 0x12345678, which triggers EC_DATA_ABORT_LO. Kernel logs, kills this task, keeps others running.
**Actions**:
1. Print "[Task C] about to deref a bad pointer at 0x12345678 (expect kill)\n"
2. volatile uint64_t *bad = (volatile uint64_t *)0x12345678ULL; *bad = 0xDEADBEEFCAFEBABEULL;
3. (Never reaches next line; kernel kills task)

```c
static void netd(void);
```
**Purpose**: EL1 kernel-mode background pinger. Runs in kernel context (no per-task TTBR0), periodically sends ICMP pings to slirp gateway, measures reply latency.
**Actions**:
1. Print "[netd] starting (kernel-mode background pinger)"
2. Initialize seq = 2 (seq 1 sent during pci_virtio_net_init)
3. Loop forever:
   - sleep_ms(5000)
   - Drain RX queue via net_rx_poll(buf, 256) until empty
   - Log drained frame count if > 0
   - t0 = timer_get_ticks()
   - net_send_ping(seq)
   - Wait up to 2M spin iterations for matching ICMP echo reply:
     - Call net_rx_poll(buf, 256)
     - Check frame >= 14+20+8 bytes (Ethernet + IP + ICMP)
     - Verify buf[12:13] == 0x0800 (IPv4)
     - Verify ip[9] == 1 (ICMP protocol)
     - Verify icmp[0] == 0 (echo reply, not echo request)
     - Verify reply_seq == seq
   - If found: t1 = timer_get_ticks(), log "[netd] ping seq=%d reply ttl=%d in %d ticks\n"
   - If timeout: log "[netd] ping seq=%d — no reply\n"
   - seq++

## Hardware Constants and Register Offsets

### Memory Layout

| Region | Range | Size | Purpose |
|--------|-------|------|---------|
| Lower Physical RAM | 0x40000000 — 0x240000000 | 8 GB | User programs, heap, stacks, page tables |
| UART | 0x09000000 — 0x0900FFFF | 64 KB | PL011 UART (base 0x09000000) |
| GIC Distributor | 0x08000000 — 0x0800FFFF | 64 KB | GICD_* registers |
| GIC Redistributor | 0x080A0000 — 0x080AFFFF | 64 KB | GICR_* registers (per-CPU) |
| PCI CONFIG | 0x10000000 — 0x1FFFFFFF | 256 MB | PCI MMIO space |
| VirtIO Devices | 0x0A000000+ | Variable | RNG, block, net, balloon, console |
| Upper-Half Kernel VA | 0xFFFF000000000000+ | Unmapped | Identity mapped via TTBR1 with KERNEL_VA_OFFSET |

### UART (PL011 @ 0x09000000)

```
UART_BASE           = 0x09000000
UART_DR (offset 0x00)   = Data Register
UART_FR (offset 0x18)   = Flag Register (bit 5 = TXFF, bit 4 = RXFE)
UART_IBRD (offset 0x24) = Integer Baud Rate Divisor
UART_FBRD (offset 0x28) = Fractional Baud Rate Divisor
UART_LCRH (offset 0x2C) = Line Control High (FIFO enable, word length)
UART_CR (offset 0x30)   = Control (enable UART, TX, RX)
UART_ICR (offset 0x44)  = Interrupt Clear Register
```

### GIC (Generic Interrupt Controller v3)

```
GICD_BASE = 0x08000000
GICD_CTLR (offset 0x0000)     = Distributor Control
  GICD_CTLR_ENABLE_G1NS = (1U << 1)  # Enable Group 1 Non-Secure IRQs
  GICD_CTLR_ARE_NS = (1U << 4)       # Address Routing Enable Non-Secure
GICD_ISENABLER (offset 0x0100) = Interrupt Set-Enable Register

GICR_BASE = 0x080A0000
GICR_WAKER (offset 0x0014) = Redistributor Waker
  GICR_WAKER_PROCESSOR_SLEEP = (1U << 1)    # CPU sleep request
  GICR_WAKER_CHILDREN_ASLEEP = (1U << 2)    # Children asleep
GICR_SGI_BASE (offset 0x10000) = SGI/PPI base (per-CPU)
GICR_IGROUPR0 (SGI_BASE + 0x0080)    = Interrupt Group Register
GICR_IGRPMODR0 (SGI_BASE + 0x0D00)   = Interrupt Group Modifier
GICR_ISENABLER0 (SGI_BASE + 0x0100)  = Interrupt Set-Enable Register
GIC_INTID_NO_PENDING = 1023 (spurious/no interrupt)
TIMER_PPI_INTID = 30 (Virtual timer PPI)
```

### MMU / Virtual Address Layout

```
KERNEL_VA_OFFSET = 0xFFFF000000000000

Page Table Indices (4 KB granule, 48-bit output address):
  L0_INDEX(va) = (va >> 39) & 0x1FF  # [39:39] ... [47:39]
  L1_INDEX(va) = (va >> 30) & 0x1FF  # [30:38]
  L2_INDEX(va) = (va >> 21) & 0x1FF  # [21:29]
  L3_INDEX(va) = (va >> 12) & 0x1FF  # [12:20]

PTE Bits (for block/page descriptors):
  PTE_VALID = (1ULL << 0)             # Bit 0: Table/page valid
  PTE_TABLE = (1ULL << 1)             # Bit 1: Table descriptor
  PTE_BLOCK = (0ULL << 1)             # Block descriptor
  PTE_AF = (1ULL << 10)               # Bit 10: Access Flag
  PTE_SH_INNER = (3ULL << 8)          # Bits 9:8: Inner shareable
  PTE_AP_RW = (0ULL << 6)             # Bits 7:6: EL1 RW, EL0 no access
  PTE_AP_RW_EL0 = (1ULL << 6)         # EL1 RW, EL0 RW
  PTE_AP_RO = (2ULL << 6)             # EL1 RO, EL0 no access
  PTE_AP_RO_EL0 = (3ULL << 6)         # EL1 RO, EL0 RO
  PTE_ATTRIDX(idx) = ((idx) << 2)    # Bits 4:2: Memory attribute index
  PTE_UXN = (1ULL << 54)              # Bit 54: User execute never
  PTE_PXN = (1ULL << 53)              # Bit 53: Privilege execute never
  PTE_NG = (1ULL << 11)               # Bit 11: Non-Global (ASID-tagged)
  PTE_ADDR_MASK = 0x0000FFFFFFFFF000ULL  # Bits 47:12 (output address)

TTBR_EL1 Layout (with 16-bit ASID):
  Bits [63:48] = ASID (16-bit address-space ID, set via ttbr_pack)
  Bits [47:1]  = Page-table base address (page-aligned, [11:0] = 0)
  Bit [0]      = CnP (common not-private, left 0)
  ttbr_pack(baddr, asid) = (baddr & TTBR_BADDR_MASK) | ((uint64_t)asid << 48)
  TTBR_BADDR_MASK = 0x0000FFFFFFFFFFFFULL
  TTBR_ASID_SHIFT = 48

User Address Space (TTBR0):
  USER_TEXT_BASE = 0x00400000ULL   # 4 MB — ELF .text/.rodata load point
  USER_STACK_TOP = 0x00800000ULL   # 8 MB — top of user stack (grows down)
  USER_STACK_PAGES = 4              # 16 KiB initial stack
  USER_STACK_PAGES_MAX = 64         # 256 KiB max after demand paging
  USER_STACK_GROWN_MAX = 60         # USER_STACK_PAGES_MAX - USER_STACK_PAGES

Block/Page Sizes:
  _512GB = 0x8000000000ULL (L0 block, 512 GB)
  _1GB = 0x40000000ULL (L1 block, 1 GB)
  _2MB = 0x200000ULL (L2 block, 2 MB)
  PAGE_SIZE = 4096 (L3 page, 4 KB)
  PAGE_SHIFT = 12
```

### Physical Memory Manager (PMM)

```
MEM_START = 0x40000000ULL  # QEMU default DRAM base
MEM_SIZE = 8ULL * 1024 * 1024 * 1024  # 8 GB

PAGE_SIZE = 4096
PAGE_SHIFT = 12

Bitmap indexing (64 bits per uint64_t word):
  BITMAP_INDEX(pfn) = (pfn) / 64        # Which uint64_t word
  BITMAP_BIT(pfn) = (pfn) % 64          # Which bit in word
  PFN_TO_PHYS(pfn) = (uint64_t)(pfn) << PAGE_SHIFT
  PHYS_TO_PFN(addr) = (uint64_t)(addr) >> PAGE_SHIFT
  PAGE_ALIGN_UP(addr) = (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
  PAGE_ALIGN_DOWN(addr) = ((addr) & ~(PAGE_SIZE - 1))
```

### Kernel Heap

```
HEAP_INITIAL_PAGES = 256        # 1 MB initial heap
PAGE_SIZE = 4096

Block Header Layout:
  struct block_header_t {
    size_t size;                # Usable payload size (excludes header)
    uint32_t magic;             # BLOCK_MAGIC_ALLOC or BLOCK_MAGIC_FREE
    uint32_t is_free;           # 1 = free, 0 = allocated (redundant with magic)
    struct block_header_t *next; # Next block in address-order list
  }
  BLOCK_MAGIC_ALLOC = 0xA110CEDUL
  BLOCK_MAGIC_FREE = 0xFEEDF1EEUL
  BLOCK_HEADER_SIZE = HEAP_ALIGN_UP(sizeof(block_header_t))  # 16 bytes
  HEAP_ALIGN = 16  # All allocations 16-byte aligned
  HEAP_ALIGN_UP(x) = (((x) + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1))
```

### Task Scheduler

```
TASK_STACK_PAGES = 4          # 16 KiB per task kernel stack
USER_STACK_PAGES = 4          # 16 KiB initial user stack
USER_STACK_PAGES_MAX = 64     # 256 KiB max total user stack
USER_STACK_GROWN_MAX = 60     # Extra pages available for demand paging

task_t Layout (aarch64):
  Offset 0:   uint64_t sp;              # Kernel SP (saved by context_switch)
  Offset 8:   uint64_t pid;
  Offset 16:  task_state_t state;       # TASK_READY / RUNNING / SLEEPING / DEAD
  Offset 24:  uint64_t sleep_until;
  Offset 32:  uintptr_t stack_phys;     # Kernel stack phys base
  Offset 40:  uint64_t ttbr0;           # User page table (MUST be at +40 for switch.S)
  Offset 48:  uint64_t user_sp;         # SP_EL0
  Offset 56:  uintptr_t kstack_top;
  Offset 64:  uintptr_t ustack_phys;
  Offset 72:  elf_image_t exec_image;   # Per-PT_LOAD allocations (for exec'd tasks)
  Offset 88:  char name[16];
  Offset 104: struct fd_table *fds;
  Offset 112: struct task *next;        # Run queue link
  Offset 120: uintptr_t stack_grown_phys[60];  # Demand-paged stack pages
  Offset 600: uint16_t stack_grown_count;

task_state_t enum:
  TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_DEAD

ASID Allocation:
  Range [1, 65535] (ASID 0 reserved for kernel/idle)
  On wraparound: global TLB flush + counter reset
```

### Syscall Numbers

```
SYS_READ = 0
SYS_WRITE = 1
SYS_OPEN = 2
SYS_CLOSE = 3
SYS_EXIT = 4
SYS_YIELD = 5
SYS_SLEEP = 6
SYS_GETPID = 7
SYS_LSEEK = 8
SYS_UPTIME = 9
SYS_NET_PING = 10        # arg0 = seq; returns reply TTL or -1
SYS_KILL = 11            # arg0 = pid; returns 0 or -1
SYS_FORK = 12            # child sees 0, parent sees child pid
SYS_EXEC = 13            # arg0 = path, arg1 = argv; noreturn on success
SYS_BALLOON = 14         # arg0 = op, arg1 = n

Balloon Sub-Operations:
  BALLOON_OP_INFLATE = 0    # Hand pages to host
  BALLOON_OP_DEFLATE = 1    # Reclaim pages from host
  BALLOON_OP_ACTUAL = 2     # Query current size
  BALLOON_OP_TARGET = 3     # Query host target
```

### Exception Syndrome Register (ESR_EL1)

```
ESR_EC_SHIFT = 26
ESR_EC_MASK = (0x3FULL << 26)
ESR_EC(esr) = ((esr) >> 26) & 0x3F

Exception Classes:
  EC_UNKNOWN = 0x00
  EC_WF_TRAPPED = 0x01
  EC_SVC_AARCH64 = 0x15          # Syscall trap (SVC #0)
  EC_HVC_AARCH64 = 0x16          # Hypervisor call (HVC #0)
  EC_SMC_AARCH64 = 0x17          # Secure monitor call
  EC_INST_ABORT_LO = 0x20        # Instruction abort from lower EL
  EC_INST_ABORT_CUR = 0x21       # Instruction abort from same EL
  EC_PC_ALIGN = 0x22             # PC alignment fault
  EC_DATA_ABORT_LO = 0x24        # Data abort from lower EL (EL0)
  EC_DATA_ABORT_CUR = 0x25       # Data abort from same EL
  EC_SP_ALIGN = 0x26             # SP alignment fault
  EC_FP_AARCH64 = 0x2C           # FP/SIMD exception
  EC_SERROR = 0x2F               # System error
  EC_BRK = 0x3C                  # Software breakpoint

ISS (Instruction-Specific Syndrome) for Data/Instruction Abort:
  ESR_ISS_DFSC(esr) = (esr) & 0x3F         # [5:0] Fault Status Code
    0x05 = Translation fault (L1)
    0x06 = Translation fault (L2)
    0x07 = Translation fault (L3)
  ESR_ISS_WNR(esr) = ((esr) >> 6) & 0x1   # [6] Write not Read
  ESR_ISS_CM(esr) = ((esr) >> 8) & 0x1    # [8] Cache maintenance
  ESR_ISS_S1PTW(esr) = ((esr) >> 7) & 0x1 # [7] Stage-1 fault during page-table walk
  ESR_ISS_EA(esr) = ((esr) >> 9) & 0x1    # [9] External abort
```

### Timer (ARM Generic Timer)

```
TIMER_PPI_INTID = 30       # Virtual timer interrupt (PPI)
TIMER_INTERVAL_MS = 10     # 10 ms periodic tick

ARM Generic Timer Registers (aarch64 sysregs):
  CNTFRQ_EL0      # Counter frequency (Hz) — read-only
  CNTV_TVAL_EL0   # Virtual timer compare value (counts down)
  CNTV_CTL_EL0    # Virtual timer control (bit 0 = enable, bit 1 = mask)
  CNTVCT_EL0      # Virtual counter read (monotonic, ticks at CNTFRQ_EL0)
  CNTP_CVAL_EL1   # Physical timer compare value (EL1 only)
  CNTPS_TVAL_EL1  # Secure physical timer (EL1 only)
```

### CPU Information (MIDR, Cache, Cycles)

```
MIDR_EL1     # Main ID Register (architecture version, part number, implementer)
PMCCNTR_EL0  # Performance Monitor Cycle Counter (64-bit, wraps at 2^64)
PMCNTENSET_EL0 # PMU Counter Enable Set (bit 31 = cycle counter)
```

## Rust Porting Strategy

### Module Structure

```
kernel/
  ├── lib.rs                      # Public API exports
  ├── early_boot.rs              # early_init(), zero_bss(), enable_fp_simd()
  ├── main.rs                     # kernel_main()
  ├── user_shell.rs              # task_shell(), shell commands, user helpers
  ├── syscall_wrappers.rs        # sys_read, sys_write, etc. (inline asm)
  ├── task_demos.rs              # task_a(), task_b(), task_crash()
  ├── netd_daemon.rs             # netd()
  └── asm/
      ├── lib.rs                 # Asm exports
      └── early_boot.s           # _start, physical relocation (keep as-is from boot.S)
```

### Type Design

```rust
// Early boot phases tracked via TypeState pattern
pub struct EarlyBoot {
    _phase: PhantomData<EarlyPhase>,
}

pub struct BootPhase1 { /* BSS zeroed, FP enabled */ }
pub struct BootPhase2 { /* UART + exceptions ready */ }
pub struct BootPhase3 { /* PMM + MMU enabled */ }
pub struct BootPhase4 { /* Upper-half running */ }

// Kernel state singleton
pub static KERNEL: KernelState = KernelState { ... };

pub struct KernelState {
    uart: UartDriver,
    pmm: PhysicalMemoryManager,
    mmu: MemoryManagementUnit,
    scheduler: Scheduler,
    timer: GenericTimer,
    gic: GicController,
    devices: DeviceRegistry,
    // ... etc
}

// Syscall dispatch (from exception context)
pub fn syscall_dispatch(frame: &mut TrapFrame) {
    match frame.x8 {
        SYS_READ => handle_sys_read(frame),
        SYS_WRITE => handle_sys_write(frame),
        // ...
    }
}

// User-space syscall wrappers (must use inline asm for SVC instruction)
#[inline(always)]
pub fn sys_read(fd: i32, buf: *mut u8, count: u64) -> i64 {
    let result: i64;
    unsafe {
        asm!(
            "svc #0",
            inout("x0") fd as u64 => result,
            in("x1") buf,
            in("x2") count,
            in("x8") SYS_READ as u64,
            clobber_abi("C"),
        );
    }
    result
}
```

### Locking and Statics

- **UartDriver**: Use `Mutex<UartState>` or volatile writes depending on single-threaded guarantee
- **Scheduler**: `Mutex<RunQueue>` for task list (coarse-grained; context_switch is atomic in asm)
- **Timer**: `AtomicU64` for tick counter; `Mutex<Vec<TimerCallback>>` for callbacks
- **GIC**: `Mutex<GicState>` for IRQ enable bitmap
- **PMM**: `Mutex<PageAllocator>` for bitmap; lock held only during alloc/free
- **Heap**: `Mutex<HeapAllocator>` for block list

### Assembly Requirements

**MUST stay assembly** (cannot be written in Rust):

1. **boot.S → early_init linking**: PC-relative branching in physical address space requires raw code. Keep src/boot.S as-is.
2. **context_switch (sched/switch.S)**: Register save/restore, TTBR0/TTBR1 swap, ASID load — instruction ordering and volatile semantics require asm. Platform-specific (aarch64) and timing-sensitive.
3. **Exception vector (exception/vector.S)**: EL1 → EL0 transitions, FP state save, exception return (ERET) — must preserve exception state and ISB/DSB semantics.
4. **SYSREG access for CP CTL**: CPACR_EL1 read/modify/write, ISB after (enable_fp_simd).
5. **HVC #0 for PSCI**: SYSTEM_RESET callout in reboot command.
6. **fork_return stub**: Return path after fork that sets x0=0 and ereturns to user space.
7. **TLB maintenance (TLBI VAE1)**: Focused invalidate after stack growth mapping.

**Candidates for Rust + inline asm**:

1. **Syscall wrappers (sys_read, sys_write, etc.)**: Use inline asm! macro in Rust; no function call overhead.
2. **Timer register access**: CNTFRQ, CNTVCT, CNTV_TVAL via inline asm (mrs/msr).
3. **GIC register access**: MMIO reads/writes can be wrapped in unsafe { }; logic is straightforward.
4. **Page table walk**: MMU page descriptor traversal — pure Rust with raw pointer dereferencing; no timing constraints.

### Volatile Access Pattern

```rust
// Kernel MMIO (upper-half access via KERNEL_VA_OFFSET)
pub fn mmio_read32(addr: usize) -> u32 {
    unsafe { core::ptr::read_volatile(addr as *const u32) }
}

pub fn mmio_write32(addr: usize, val: u32) {
    unsafe { core::ptr::write_volatile(addr as *mut u32, val) }
}

// Register field access (struct repr(C) with volatile fields)
#[repr(C)]
pub struct UartRegs {
    pub dr: u32,           // +0x00
    _reserved1: [u32; 5],  // +0x04..+0x17
    pub fr: u32,           // +0x18
    // ...
}

// Read: ptr::read_volatile(&(*uart_base as *const UartRegs).dr)
// Write: ptr::write_volatile(&mut (*uart_base as *mut UartRegs).dr, val)
```

## Boot Sequence Ordering (Critical Constraints)

1. **Physical early_init (before MMU)**:
   - Zero BSS: Clears .bss for kernel globals
   - Enable FP/SIMD: Set CPACR_EL1.FPEN (varargs use SIMD regs)
   - UART init: Establish logging output (must be first so uart_println works)
   - Exceptions init: Install physical-address exception vectors
   - PMM init: Set up page bitmap (required for subsequent alloc)
   - MMU init: Enable TTBR1 (upper-half kernel mapping), run tests while TTBR0 is identity table
   - Print "[BOOT] MMU Enabled" (UART still at physical 0x09000000 during ID-mapped period)

2. **Upper-half kernel_main**:
   - Relocate MMIO: Switch UART base to upper-half VA
   - Relocate exceptions: Reload VBAR_EL1 to upper-half
   - Relocate PMM bitmap: Move to upper-half TTBR1 mapping
   - CPU init: Read MIDR, set up cycle counter
   - Heap init: Allocate 1 MB initial heap (requires working PMM + TTBR1 mapping)
   - GIC init: Configure IRQ routing (requires heap for structures)
   - PCI enumerate: Discover VirtIO devices (requires heap)
   - VirtIO device init: RNG, block, net, balloon, console (net sends seq=1 ping)
   - FAT32 mount: From block device
   - VFS init: Root directory + mount points
   - Devices register: Create /dev/* nodes
   - Process init: Initialize /proc backing
   - Scheduler init: Create idle task
   - Task spawning: task_a, task_b, task_shell, task_crash, netd (in order)
   - Timer start: Program 10 ms periodic (enables TIMER_PPI_INTID)
   - Idle loop: WFI forever

**Ordering Rationale**:
- UART first: All subsequent logging depends on it
- Exceptions before PMM: Exception handlers may be needed during PMM init
- PMM before MMU: MMU test and task stack allocation need page frames
- MMU before heap: Heap needs upper-half TTBR1 mapping to access bitmap
- GIC before devices: Device IRQs won't be routed without GIC config
- Scheduler before tasks: Run queue must exist before task_create
- Tasks before timer: Timer IRQ will context-switch; need valid task pointers
- Timer last: Sets up the interrupt that drives preemption

## Gotchas and Correctness Issues

1. **ASID Wraparound**: After 65535 task creations, ASID counter wraps. Must do global TLB flush before recycling (stale TLB entries for old ASID values would alias). Code in sched_asid_alloc() checks wrap and calls mmu_flush_tlb_all().

2. **Demand-Paged Stack Growth**: Stack fault at VA `F` triggers sched_try_grow_stack(t, F). Must validate `F` is in zone `[USER_STACK_TOP - MAX*PAGE, USER_STACK_TOP - INITIAL*PAGE)` and stack_grown_count < MAX_GROWN. Allocate page, zero via PHYS_TO_VIRT (TTBR1 so doesn't depend on TTBR0), map with nG=1 and PTE_UXN, issue TLBI VAE1 for (VA, ASID), then eret resumes faulting instruction.

3. **TTBR0 Identity vs. Personal**: Early_init sets up TTBR0 as identity table (VA == PA) for lower 1 GB. After MMU enable, mmu_run_tests runs while TTBR0 still points here — safe window for fresh PTEs in L0_table_lo before any per-task user_l0 takes over TTBR0.

4. **Fork Child x0 = 0**: sched_fork deep-copies parent task_t, but modifies child's trap_frame so that when fork_return ereturns, x0 is 0 (child sees 0, parent sees child pid). Parent's frame is the one passed in; child's frame is created on child's kstack.

5. **Exec Noreturn**: sys_exec loads ELF, clears old image, sets new user entry point, zaps TTBR0+SP, calls task_entry(). If it returns (ELF load fails), frame.x0 = error code and eret returns to user shell. If it succeeds, task never returns.

6. **Demand-Paged Stack: Per-Page Tracking**: stack_grown_phys[] array records physical bases of lazily-allocated pages. sched_reap() walks the array to pmm_free each one individually, not just pmm_free_pages (which requires contiguous range). This handles fragmentation.

7. **Shell Command Parsing**: User-space task_shell hand-tokenizes commands (space-delimited) in-place by NUL-terminating argv words. No bounds check on argc — must cap at 15 (argv[16] with NULL terminator). Prevents argv array overflow.

8. **Task Exit in Kernel Context**: Both sys_exit and task_shell "exit" command call sys_exit via SVC, which traps to syscall_dispatch → handle_sys_exit → sched_kill_task(current) → sched_reap. If called from netd (EL1 kernel task), falls through to sched_reap which only schedules the reaper — netd should not return. netd implementation loops forever, so task_exit is unreachable.

9. **Balloon Page Count**: sys_balloon(INFLATE/DEFLATE, N) hands N pages to/from host. Host enforces VIRTIO_BALLOON_MAX_PAGES cap; if PMM exhausted, balloon driver clamps. Caller receives actual count processed (may be < N).

10. **Ping Sequence Number**: Shell ping uses static seq counter starting at 100 to avoid colliding with netd (seq 2+). Each shell ping increments seq. If shell and netd ping simultaneously, netd's reply might be mismatched (wrong seq) and timeout.

11. **IRQ Dispatch and Scheduler Lock**: gic_ack_irq() returns INTID. Timer IRQ (INTID 30) routes to timer_handle_irq(), which calls sched_wake_sleepers() and schedule(). Context_switch is atomic in asm; Scheduler::lock is not held during this. If user task calls sys_sleep while another CPU's timer fires, sched_wake_sleepers might mark it READY before sys_sleep returns. On uniprocessor (hobby kernel), this is safe; on SMP, would need care.

12. **User Stack Allocation**: sched_create_task allocates USER_STACK_PAGES (4 pages = 16 KiB) contiguously for the initial stack via pmm_allocate_pages. When demand-paging triggers, sched_try_grow_stack allocates individual pages. If PMM becomes exhausted between initial alloc and first growth fault, the fault will fail and the task will be killed. No pre-reservation.

13. **Console / UART Contention**: Both sys_write(1, ...) and uart_println use the same PL011 UART. Kernel code calls uart_println; user tasks call sys_write. In SVC handler, we must either serialize (lock UART) or ensure atomicity at small packet sizes. Current code does not explicitly lock, relying on UART FIFO and single-threaded execution between IRQs.

14. **MMU Flush During Task Creation**: Each new user task gets a fresh ASID (sched_asid_alloc). If ASID wraps, mmu_flush_tlb_all() is called. However, if a task is created during task execution (e.g., fork), this might invalidate the running task's TLB entries! In practice, fork() is a syscall (SVC trap), so kernel runs; TLB flush is safe. But subtle.

15. **Netd and RX Drain Race**: netd drains net_rx_poll in a loop before sending a ping. If another task (task_shell ping) sends a ping concurrently, netd might drain that reply and miss its own. No synchronization between netd and task_shell ping. Works by luck (separate seq numbers and timeout logic).

## Integration Points (Who Calls the Kernel Module)

The kernel module is the top-level orchestrator. It calls every other subsystem but is rarely called back. Exception:
- **Scheduler calls exception handlers** on IRQ/fault: exception.c calls back into sched via sched_wake_sleepers, sched_kill_task, etc.
- **Device drivers register IRQ handlers** with GIC: kernel_main calls gic_enable_irq; timer/netd register callbacks.
- **Syscall dispatch calls subsystem handlers**: syscall_dispatch (called from exception context) dispatches to VFS, scheduler, networking, etc.
- **Idle task (scheduler) is "called" by kernel_main via context_switch**: kernel_main never directly calls any user task entry.

## Public Exports from kernel.c

```c
// Early boot (called from boot.S)
void early_init(void)
void kernel_panic_return(void)
void kernel_main(void)
```

All other functions (sys_*, task_*, netd, shell_*, u_*) are static or inline and not exported. Syscall numbers (SYS_* constants) are defined in syscall.h. Task entry points are passed as function pointers to sched_create_task.

