# Fermi OS

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel built from scratch in `C` and assembly, targeting QEMU's `virt` machine with a Cortex-A72 processor.


---

## Features

### Boot & Memory
- **PL011 UART Driver** — Full serial I/O, hex/decimal/binary output and formatted print with `%s %d %u %x %p %b %c %%` format specifiers
- **Physical Memory Manager (PMM)** — Bitmap-based page allocator managing 8 GB of RAM, with single and contiguous multi-page allocation
- **MMU (Memory Management Unit)** — 3-level page tables (L0→L1→L2) with 2 MB blocks, 48-bit virtual address space, 4 KB granule
- **Higher-Half Kernel** — Kernel linked at Virt Memory Address `0xFFFF_0000_4000_0000` with physical Load Memory Address `0x4000_0000`. Dual address space with TTBR0 (user) and TTBR1 (kernel)
- **Kernel Heap** — First-fit allocator with block splitting, coalescing, double-free detection, and bounds checking (`kmalloc`/`kfree`)
- **Kernel Panic Handler** — System register dump and CPU halt on unrecoverable errors

### Exceptions & Interrupts
- **Exception Handling** — Full ARMv8-A vector table, trap frame save/restore, ESR decoding, register dump on fault
- **GICv3 Interrupt Controller** — Minimal GICv3 bringup with Distributor/Redistributor initialization, affinity routing, system register interface, IRQ acknowledge/EOI
- **ARM Generic Timer** — Configurable periodic tick (default 1 s) driving the scheduler, routed through GICv3 PPI

### Scheduling & Processes
- **Preemptive Scheduler** — Round-robin task scheduler with timer-driven preemption, per-task kernel stacks, context switching via callee-saved register save/restore, task creation/exit/reaping lifecycle, and a circular run queue
- **Task Sleep** — Tick-based voluntary sleep via `sleep_ms(ms)`, with per-task deadlines and automatic wakeup on timer IRQ (`sched_wake_sleepers`)
- **EL0 User-Space Tasks** — Full EL1→EL0 privilege separation via `eret`, per-task user text and stack mappings with proper permission bits (UXN, PXN, AP), separate kernel and user stacks, and a trampoline that sets `SP_EL0`/`ELR_EL1`/`SPSR_EL1` before dropping to user mode
- **Per-Task Address Spaces (TTBR0 Switching)** — Each task gets its own L0→L1→L2 user page tables allocated from the PMM; `context_switch` swaps `TTBR0_EL1` and performs `TLBI`/`DSB`/`ISB` on every task switch, with recursive page table teardown on task reap
- **System Call Interface (SVC)** — AAPCS64-based `svc #0` dispatch with `x8` as syscall number and `x0`–`x7` for arguments; return value written back via the trap frame
- **POSIX-style Syscalls** — `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_EXIT`, `SYS_YIELD`, `SYS_SLEEP`, `SYS_GETPID`, `SYS_LSEEK`, `SYS_UPTIME`, `SYS_NET_PING`, `SYS_KILL`, `SYS_WAITPID` (pinned to Linux aarch64 number 260). I/O syscalls route through the current task's fd table; the dispatch path unmasks IRQs so blocking syscalls don't starve other tasks. User pointers are range-checked against `[0, USER_VA_TOP)` to close kernel-pointer-injection holes
- **POSIX `errno`** — Per-task error codes, not one global. Each task owns a TLS page at `USER_TLS_BASE` and `TPIDR_EL0` points at it; `errno` is the `int` at offset 0 — the same shape Linux uses (thread pointer in `TPIDR_EL0`, errno at the TCB base), so a libc can implement `__errno_location()` directly. `context_switch` reloads `TPIDR_EL0` on **every** switch, not just at exception entry: a syscall that blocks mid-way context-switches away and must find the register correct when it finally resumes. The kernel stores errno through the `PHYS_TO_VIRT` alias so the write works regardless of which task's `TTBR0` is loaded. Failures return `-1` *and* set a meaningful code (`EOPNOTSUPP` for features deliberately unsupported, `ENOMEM` for address-space exhaustion, `EINVAL` for bad arguments); unknown syscalls return `ENOSYS` so a libc can feature-probe. Successful calls leave errno untouched, as POSIX requires
- **Syscall-level verification** — In-kernel assertions plus `user/errno.c`, a real `exec`'d ELF64 binary linked against the public `<sys.h>` headers. That exercises the whole chain (ELF loader → exec-time TLS allocation → user-side `TPIDR_EL0` accessor) rather than the kernel's internal view of errno: 18 checks covering that failures set the right code, that success leaves errno alone, that user space can write it, and that a forked child's errno is independent
- **Virtual Memory Areas + `mmap`** — Anonymous `mmap`/`munmap`/`mprotect` with per-task VMA bookkeeping (`src/mm/vma/`). Regions start empty and are **demand-paged**: no physical page is allocated until the region is first touched, so `mmap` is O(1) in physical memory regardless of size. The demand-fault handler (`sched_try_handle_vma_fault`) checks the faulting access against the VMA's `PROT_*` bits, so a write to a read-only region still faults instead of being silently satisfied. Backing pages are recorded per-VMA and released on `munmap` or task reap. `mprotect` rewrites both live PTEs and the VMA record, so pages faulted in later get the new protection too. Syscall numbers deliberately match Linux aarch64 (`mmap`=222, `munmap`=215, `mprotect`=226, `brk`=214)
- **VMA Split & Merge** — `munmap` and `mprotect` accept partial ranges, as POSIX requires. `vma_split_at()` cuts a VMA in two, giving the tail half its own page array; `vma_merge_adjacent()` welds neighbours back together when they are contiguous and share protection, so a long alloc/free loop doesn't consume a VMA slot per iteration. Both are atomic: the page array is allocated before any list entry is touched, so a failure leaves the set unmodified
- **`brk` / `sbrk` Heap** — A contiguous program break backed by a fixed per-task 16 MiB arena reservation at `USER_BRK_BASE`. Growth extends the VMA in place and commits no physical memory until the heap is touched; shrinking is refused rather than unmapped, so a stale pointer below the new break keeps working. Returns the *current* break on failure, matching Linux rather than an errno
  - Deliberate current limits: `mmap` is anonymous-private only (file-backed needs a page cache), and `MAP_SHARED` is **refused rather than silently downgraded** to private — downgrading would produce a program that appears to work and silently loses data
- **`fork` Memory Isolation** — A forked child gets a genuine private copy of its address space: `vma_clone()` duplicates the VMA records, then each faulted-in page is re-allocated and `memcpy`'d into the child's page tables, with the child's break inherited from the parent. Pages the parent never touched are left unmapped in the child, so each task demand-faults its own zero page independently. The read-only `.text` window is the one region *shared* — safe by permission rather than by luck, since those pages are EL0 read-only and neither side can write them. Demand-grown stack pages are copied too, so a child doesn't lose stack its parent had faulted in
  - This is an **eager copy, not lazy copy-on-write**. Real COW would map both spaces read-only and copy on the first write fault, which is the better design but needs a per-page refcount and write-fault recovery — and this kernel's fault path currently treats every write fault as fatal. Until that exists, copying is the only way to get the correctness POSIX demands
- **`fork` fd Inheritance** — A forked child inherits its parent's descriptors rather than getting a fresh console-only table. Because the inherited slots share one refcounted open file description, a raw `read()` in the child advances the parent's file offset too, and a `close()` in the child leaves the parent's descriptor working — both are POSIX requirements that a copied table gets wrong. Buffered stdio is *not* shared: the child's `FILE` buffer is a copy in the child's own memory, so a read it satisfies from that buffer correctly leaves the shared offset alone
- **`waitpid` / `wait` and Exit Status** — `SYS_WAITPID` collects an exited child and returns its pid. `sched_waitpid` separates the two cases POSIX treats completely differently: no matching child is an immediate `ECHILD`, while a child that has *not exited yet* means the caller must **block** (`yield()` until the child is scheduled and exits). `WNOHANG` returns 0 instead of blocking. Collapsing those two into one answer breaks the most common sequence there is, `fork()` immediately followed by `wait()` — the parent usually asks before the child has ever run
  - A task whose parent has not collected it becomes a **zombie** (`TASK_ZOMBIE`, held on the dead list) rather than being freed by `sched_reap()`, which would throw away the exit status. Orphaned children are detected on reap and re-parented so they can't accumulate forever
  - The status word follows POSIX exactly: exit code in bits 8-15 for a normal exit, signal in bits 0-6 with `0x80` set for a signalled one. A fault death goes through the *signalled* encoding as `SIGSEGV` — packing it into bits 8-15 would give `0xFF00`, whose low 7 bits are still zero, so `WIFEXITED()` would report a segfault as "exited normally". There is a real signal 11 behind it, so the stock `WIFSIGNALED`/`WTERMSIG` macros work with no Fermi-specific encoding
  - Deliberate current limits: negative pids below `-1` (process groups) are refused rather than silently reinterpreted, and blocking waits consume CPU instead of parking the task on a wait queue — nothing can *wake* such a queue yet, because child-exit notification needs signals
- **EL0 Page-Fault Handling** — Data and Instruction aborts from a lower EL kill *only* the offending task (logging pid/ELR/FAR/ESR) and continue scheduling; aborts from EL1 still trip a full `kernel_panic` since they indicate a real kernel bug. Instruction aborts never fault a page into existence, so a `UXN` violation stays fatal
- **Demand-Paged User Stack** — Stack faults below the initial region grow the stack on demand up to a 256 KiB cap, one page at a time, with the TLB invalidated for the specific `(VA, ASID)` pair
- **Kernel-Mode (EL1) Tasks** — `sched_create_kernel_task` provides a parallel scheduler path with its own `kernel_task_trampoline` (no `eret`, no TTBR0 swap). Used by the in-tree `netd` daemon

### PCI & VirtIO
- **PCI Express (ECAM)** — Brute-force bus enumeration, device discovery and caching, BAR assignment (32-bit and 64-bit MMIO), Memory Space + Bus Master enable via Command register
- **VirtIO PCI Transport** — Capability list walking (vendor-specific 0x09), common/notify/ISR/device config BAR+offset resolution, MMIO register access via named offsets (virtio spec 4.1.4.3)
- **Virtqueue (Split)** — Reusable split virtqueue module with descriptor table, available ring, used ring, `VIRT_TO_PHYS` DMA address conversion, submit/notify/poll API
- **VirtIO RNG Driver** — Full VirtIO device init sequence (reset → ack → driver → feature negotiation → FEATURES_OK → queue setup → DRIVER_OK), random byte generation via `virtio-rng-pci`
- **VirtIO Block Driver** — `virtio-blk-pci` device init, capacity readout from device config, and synchronous 512-byte sector `blk_read`/`blk_write` using chained descriptors (header + data + status) over the split virtqueue
- **VirtIO Net Driver** — `virtio-net-pci` device init (modern dev id `0x1041`, `VIRTIO_F_VERSION_1` required), MAC + link-status readout, RX queue pre-fill (8 1 600-byte buffers), `net_tx`/`net_rx_poll` synchronous APIs, and per-direction packet counters

### Networking
- **Layer 2 — Ethernet** — Hand-built ARP requests/replies. Boot path sends a broadcast ARP for the slirp gateway, parses the reply, and caches the resulting `gateway_mac`
- **Layer 3 — IPv4 + ICMP echo** — RFC 1071 internet checksum helper, IPv4 header builder, and ICMP echo request that drives a full L3 round-trip to QEMU's slirp gateway (`10.0.2.2`). Verified: ARP → IPv4 → ICMP echo reply with TTL
- **netd kernel daemon** — Periodic background pinger running at EL1: drains incoming RX, sends an ICMP echo every 5 s, and prints reply latency in ticks
- **`SYS_NET_PING`** — EL0-callable ICMP echo so user-space tasks (notably the shell's `ping` command) can fire pings without leaving the syscall ABI

### Filesystem & Devices
- **Virtual Filesystem (VFS)** — Unix-style vnode tree with path resolution (`.`, `..`, multi-slash tolerant). Per-vnode `file_operations` vtable (read/write) and `vnode_operations` vtable (lazy `lookup`). Supports char devices, block devices, directories, regular files
- **Per-Process File Descriptor Table** — `fd_table_t` allocated per task, freed on reap. fd 0/1/2 auto-opened to `/dev/console` (stdin/stdout/stderr). `fd_open`/`fd_read`/`fd_write`/`fd_close`/`fd_seek` (SEEK_SET/SEEK_CUR) dispatching through vnode ops
  - Open file descriptions are **refcounted**, which is what makes `fork` correct: `fd_table_clone()` gives the child its own table whose occupied slots point at the *same* `file_t`, and `fd_close`/`fd_table_destroy` drop a reference instead of freeing. Without this, a child that closed an inherited descriptor would destroy the description the parent was still using
- **Built-in Char Devices** — `/dev/console` (UART read/write), `/dev/null` (discard/EOF), `/dev/zero` (zero-fill), `/dev/rng` (virtio-rng with bounce buffer for DMA)
- **Block Device Node** — `/dev/blk` exposing the virtio-blk disk with sector-aligned byte-offset read/write
- **FAT32 (VFS-backed)** — Mounted at `/mnt/fat32`. Lazy directory traversal: each `lookup` walks on-disk directory entries and creates a vnode on demand with per-vnode `(first_cluster, size)` state. `open`/`read` on regular files goes through the full VFS → fd → `file_operations.read` path. 8.3 short names only — there is no long-filename (LFN) support, so a file whose name exceeds 8 characters plus extension is stored by `mcopy` under a generated short name (`ERRNOT~1`) and looking it up by its full name fails. User binaries shipped on the disk image are named to fit 8.3 for this reason
- **`/proc` synthetic filesystem** — Mounted at `/proc`, regenerates content per-read from live kernel state. Files: `/proc/uptime`, `/proc/meminfo` (PMM + heap + VMA mapped/resident totals across all tasks), `/proc/tasks` (run-queue snapshot with state names), `/proc/interrupts` (per-INTID counts via the GIC dispatch hook), `/proc/netinfo` (MAC, link, IP, gateway MAC, packet counters), `/proc/cmdline`, `/proc/version`

### Linux Binary Compatibility
- **A foreign static Linux binary runs** — `user/linux/` builds `LINHELLO.ELF`: a static aarch64 executable produced by the **Linux toolchain**, linked against **nothing** (no crt0, no `libc.a`, no Fermi header), speaking only Linux syscall numbers and the Linux process-startup contract. It execs through exactly the same `sys_exec` path as our own programs, because no special case is needed: **32 checks** covering the stack entry convention, the auxiliary vector, `arch_prctl`, and the startup calls every libc makes
  - This is the milestone that makes the Linux-compat half of this project real rather than aspirational. It is deliberately a hand-written binary and not musl: proving the *ABI contract* is a prerequisite for musl working, and musl's own startup is a much larger surface (TLS setup, `set_tid_address`, `rseq`, robust lists, ifunc)
- **Dual syscall numbering** — our libc uses small Fermi numbers; a Linux binary always uses the asm-generic table. Rather than renumbering, `translate_linux_syscall()` maps Linux numbers onto the same handlers, so both ABIs coexist and the translation is one readable, auditable table. Only numbers whose **argument layout is identical** are aliased; anything that shifts or reshapes arguments (`openat`, the `stat` family, `nanosleep`) gets its own case, because an alias would silently read the wrong argument. Unlisted numbers reach the `ENOSYS` default rather than aliasing onto an unrelated Fermi call that happens to share the number
- **Linux process startup** — `exec` now builds the stack layout a Linux binary expects: `argc` at `SP_EL0`, then `argv[]`, a `NULL`-terminated `envp`, then the auxiliary vector terminated by `AT_NULL`. Previously we only handed the program `argc`/`argv` in `x0`/`x1` (AAPCS64), which is enough for our own crt0 and useless to anything else. Both conventions are now satisfied at once, so Fermi-linked and foreign binaries share one `exec` path with nothing to tell them apart
- **Auxiliary vector** — `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ`/`AT_BASE`/`AT_FLAGS`/`AT_ENTRY`/`AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`/`AT_HWCAP`/`AT_CLKTCK`/`AT_SECURE`/`AT_RANDOM`/`AT_EXECFN`. Without these a loader cannot find its interpreter, so `ld.so` is impossible without them
  - `AT_PHDR` is computed, not assumed. `e_phoff` is a *file* offset, and the virtual address is not that number — for a normal static link the first `PT_LOAD` is offset 0 at `vaddr 0x400000`, so the headers land at `vaddr + e_phoff`. It is advertised only when a `PT_LOAD` actually mapped covers the whole table; otherwise `AT_PHDR` is 0 so a guest's header walk fails loudly instead of reading whatever happens to be mapped there
  - `AT_HWCAP` is reported as **0** rather than advertising the CPU's features: this is a fixed CPU model with no runtime feature detection, and a guest that trusted a non-zero value would assume instructions never verified. `AT_BASE` is 0 because there is no `ld.so` yet
  - `AT_RANDOM` points at 16 bytes seeded from virtio-rng (zeros if the device is absent — acceptable, since the value only ever seeds canaries and hash state, never a secret)
- **`arch_prctl(ARCH_SET_FS)` / `ARCH_GET_FS`** — how a Linux aarch64 process installs its thread pointer. `SET_FS` returns **0** on success (every libc aborts on a non-zero return), `GET_FS` reports the register. Misaligned pointers are refused rather than silently rounded, and `SET_GS` is refused rather than accepted-and-ignored so a guest cannot believe it has shadow state it does not
  - This required fixing `switch.S`, which reloaded `TPIDR_EL0` from the hardcoded `USER_TLS_BASE` constant on **every** context switch. That is immune to struct layout but silently undoes any `SET_FS` the instant the task was preempted. `TPIDR_EL0` is now saved to and loaded from `task->tpidr_el0`, and the offset is asserted against the real struct layout at build time
  - The hand-maintained `task_t` offsets used by `switch.S` (`TASK_SP`, `TASK_TTBR0`, `TASK_TPIDR_EL0`) are now **numeric constants in `sched.h` with `_Static_assert`s** against `offsetof`. They were previously comments only: inserting a field mid-struct would have made the context switch read and write the wrong words, which is exactly the kind of bug that survives until a task is switched under load
- **`set_tid_address`** — returns the caller's tid. The address is recorded but nothing writes through it, since there are no threads yet; the call must still **succeed**, because glibc and musl both abort when it fails
- **Deliberate current limits** — static binaries only. Dynamic linking needs `ld.so`, ifunc/`IRELATIVE` relocations, `PT_INTERP` handling and real TLS block setup, and is not started. `openat` accepts only `AT_FDCWD`: resolving a relative path against another descriptor needs per-descriptor working directories this VFS does not track, and resolving against the wrong directory would be worse than refusing. `nanosleep`, `clock_gettime`, the `stat` family, `getdents64`, `ioctl` and `getrandom` are not yet aliased, so a Linux binary calling them gets `ENOSYS` — a truthful "not implemented" rather than a wrong answer

### Fermi libc (`user/lib/`)
- **Real archive, not header-only stubs** — `user/lib/*.c` compiles into `libc.a`, which every user program links against (`make libc` builds it alone). Deliberately an archive: a libc needs *one* definition per function, and header-only inline code silently duplicates state — a header-only malloc counter or `errno` would become a per-translation-unit copy rather than one per process
- **`<string.h>`** — `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strchr`, `strrchr`, `strstr`. Comparisons are **unsigned** by contract: a signed `char` comparison inverts the ordering of every byte >= 0x80. `memcpy` delegates to `memmove` because the kernel hands it overlapping user buffers
- **`<stdlib.h>` allocator** — First-fit free-list over the kernel's `brk()` arena, with block splitting and bidirectional coalescing (blocks form an address-ordered doubly-linked list so `free()` can merge with the previous block). Payloads are 16-byte aligned, and a header-magic check turns a double free into a diagnostic instead of free-list corruption. Provides `malloc`/`calloc`/`realloc`/`free` plus `malloc_usable_size`/`malloc_live_blocks` for testing
  - `malloc` **zeroes** its payload — stricter than POSIX, which permits indeterminate bytes. Recycled and freshly-`brk`'d memory are indistinguishable at this layer, and zeroing removes any chance of leaking one allocation's stale contents into another. Worth dropping for speed once the allocator tracks which bytes came straight from `brk`
- **`<errno.h>`** — `strerror()` for the codes this kernel actually produces, falling back to `"Unknown error"` rather than `NULL`
- **`<unistd.h>` process calls** — `open`/`close`/`read`/`write`/`lseek`, `_exit`, plus `fork()` and `getpid()`
- **`<sys/wait.h>`** — `waitpid`/`wait` with `WNOHANG`, and the POSIX status predicates `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG`/`WCOREDUMP`. Fixed-width types across the libc come from GCC's own `__INT*_TYPE__` macros rather than being spelled out in `sys.h`: spelling them out (`long long int64_t`) disagrees with the toolchain's `<stdint.h>`, which on aarch64 LP64 says `long int`, and the two collide the moment a program pulls in a real header
- **`<stdio.h>`** — Buffered `FILE` streams: `fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`/`rewind`/`fflush`/`feof`/`ferror`/`clearerr`, the character layer (`fgetc`/`getc`/`getchar`/`ungetc`/`fgets`/`fputc`/`putc`/`putchar`/`fputs`/`puts`), and `printf`/`fprintf`/`sprintf`/`snprintf`/`perror`. One buffer per stream with a single invariant — at most one of the read/write regions is ever non-empty — so "what does buf hold" is one flag rather than two interdependent counters. Requests at least as large as the buffer bypass it entirely
  - `stderr` is written through unbuffered so diagnostics can't reorder against buffered stdout
  - Position tracking keeps `base` = the file offset `buf[0]` corresponds to, which is what makes `ftell()` correct across partial fills and flushes
- **Formatted output** — One sink-agnostic core (`vformat`) that every wrapper shares, so `sprintf` and `printf` can't drift apart. Supports `%d %i %u %o %x %X %c %s %p %%` with field width, precision, the `-` `+` ` ` `0` `#` flags, `*` width/precision, and the `hh h l ll z j t` length modifiers. `snprintf` returns the length the *untruncated* result needed, per the standard
  - **Floating point (`%f %e %g %a`) is not implemented.** It needs correctly-rounded decimal expansion of a binary fraction — 128-bit integer arithmetic or a soft-float helper — so an unimplemented conversion prints a visible `<float unsupported>` marker rather than a plausible-looking wrong number
  - `open()`'s `mode` argument is fixed at `0644`: there is no variadic support in user space yet, so the caller's mode is accepted and not honoured. Documented rather than silently dropped
- **Verification** — `user/libctest.c` (shipped as `LIBCT.ELF`) is a normal exec'd program linked against the real `libc.a`: **54 checks** covering string edge cases (overlapping `memmove` in both directions, unsigned byte ordering, empty-needle `strstr`, `strrchr` last-match), allocator behaviour (alignment, zeroing on reuse, `calloc` size overflow, `realloc` grow/shrink content preservation, 200 alloc/free cycles as a coalescing check), `qsort`, and `errno`
- **`<stdio.h>` verification** — `user/stdiotest.c` (shipped as `STDIO.ELF`) is a normal exec'd program that writes ~6 KB to a real file on the FAT32 disk and reads it back: **62 checks** covering format-specifier edge cases (`%05d` placing zeros after the sign, `%.*d`, `%p`, `NULL` → `(null)`, `LLONG_MIN` not overflowing, `snprintf` returning the untruncated length), stream positioning (`SEEK_SET`/`SEEK_CUR`/`SEEK_END`, `ftell` across a partial buffer fill, rejecting a seek before the start of the file), access-mode enforcement, `ungetc`, and confirming the on-disk size and tail marker survive the round trip
- **Process-model verification** — `user/proctest.c` (shipped as `PROCT.ELF`) is a normal exec'd program: **25 checks** covering `fork` returning a child pid, a blocking `waitpid` collecting exit code 42, `WNOHANG` returning 0 for a child that is still sleeping and then reaping that same child, `ECHILD` once no children remain, fd inheritance with a genuinely shared raw file offset (the child's `read` moves the parent's `lseek` position, and the child's `close` does not break the parent's descriptor), a buffered stream staying per-process across `fork`, and a deliberately faulting child being reported as signalled with `SIGSEGV` rather than as a normal exit

### Device & Filesystem Concurrency
- **virtio-blk serialization** — The virtqueue, request header, and status byte are single-instance state, so two tasks issuing I/O at once would interleave descriptor writes and read each other's status. `blk_read`/`blk_write` take a device lock acquired with interrupts masked and released with DAIF restored, so it cannot deadlock against a timer interrupt preempting a task mid-request. Spinning briefly unmasks interrupts, since the lock holder is a different task and could not otherwise make progress. Header and status are now stack locals rather than function statics, so no unlocked path can reuse another caller's in-flight descriptor
- **FAT32 write path** — `fat32_write_at()` walks to the cluster holding a byte offset, extending the cluster chain as needed, and does read-modify-write only for partial sectors (a whole-sector overwrite skips the read entirely). The fiddly requirement is that a FAT32 file's length lives in the *directory entry*, not the cluster chain, so every write pushes the new size back to disk — a length that is correct in RAM but stale on disk produces a file that looks truncated to the next reader, including this kernel after a reopen. `open()` supports `O_CREAT`/`O_EXCL`/`O_TRUNC`/`O_APPEND`, with creation delegated to the mounted filesystem through a hook so the filesystem-agnostic VFS layer never needs on-disk knowledge
- **FAT32 per-call sector buffer** — The sector buffer was a file-scope static, so two tasks reading the disk would land their sectors in the same 512 bytes and silently corrupt each other. It is now a per-call local in each of the eight functions that touch it. This only surfaced once two tasks began doing `exec` concurrently: which of them failed varied run to run, which is the signature of a race rather than a logic bug

### EL0 Shell (interactive)
- **`task_shell`** — An EL0 task that loops reading lines from `/dev/console` (with backspace/DEL editing and echo) and dispatches built-ins. Pure user-space — talks to the kernel only via `svc`. Built-ins: `help`, `pid`, `uptime`, `ps`, `free`, `ifconfig`, `irqs`, `version`, `cat <path>`, `kill <pid>`, `top` (5× refresh tasks/mem/net), `ping`, `sleep <ms>`, `clear`, `exit`

---

## Prerequisites
> Note: This project is being developed and tested on Mac M4 chip. There is a possibility that you might encounter environment setup errors on other platforms.

Install [docker](https://www.docker.com) on your host machine.

```bash
git clone https://github.com/rituparna-ui/fermi-os.git
cd fermi-os

docker run -d -it -v .:/root/fermi-os --name osdev ubuntu
```

Once the Docker container is up and running, start a shell in the container that was just created.
```bash
docker exec -it osdev bash
```

Inside the Docker container, install required dependencies.
```bash
apt update && apt upgrade

apt install make qemu-system gcc-aarch64-linux-gnu gdb-multiarch tmux mtools dosfstools
ln -sf aarch64-linux-gnu-as /usr/bin/as
```

## Building & Running

```bash
# Build the kernel ELF
make

# Build kernel + all user programs + the disk image
make full

# Build and run in QEMU (serial console)
make run

# Build the foreign static Linux binary on its own (no Fermi libc on the link line)
make user/linux/linuxhello.elf

# Build libc.a alone, without relinking user programs
make libc

# Clean build artifacts
make clean
```

> **`make` builds only the kernel.** Editing `user/lib/*.c` relinks `libc.a`,
> but nothing rebuilds the user programs or the disk image unless you ask — so
> the stale ELF keeps running on the device and a fix appears not to work.
> Use `make full` when changing anything under `user/`.

To exit QEMU: `Ctrl-A` then `X`

## Debugging

```bash
# Launch QEMU paused + GDB in a tmux split
make tmux
```

Or manually in two terminals:

```bash
# Terminal 1: QEMU waiting for debugger
make debug

# Terminal 2: GDB connecting to QEMU
make gdb
```

### Other Utilities

```bash
# Generate compile_commands.json for clangd / IDE support
make compile_commands.json

# Dump QEMU device tree source (DTS)
make dump_dts
```
