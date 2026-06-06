#include "syscall.h"
#include "mm/mmu/mmu.h" // USER_STACK_TOP
#include "sched/sched.h"
#include "net/net.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "vfs/vfs.h"
#include "pci/virtio/balloon/balloon.h"

#include "mm/heap/heap.h"
#include "mm/pmm/pmm.h"
#include "strings/strings.h"
#include "elf.h"

#include <stddef.h>
#include <stdint.h>

// AAPCS64 syscall convention:
//   x8       = syscall number
//   x0 - x7  = up to 7 arguments
//   x0       = return value (written back into trap frame)

// User-space layout is [0, USER_STACK_TOP). Anything outside that range is
// either an unmapped TTBR0 region or — much worse — a kernel pointer the user
// is trying to trick the syscall path into dereferencing on its behalf.
//
// Limitation: these checks only validate the *range*. A user pointer inside
// the range that points to an unmapped page will still fault the kernel
// when the underlying fd op touches it. Hardening that requires either a
// kernel page-fault handler with fixup tables (Linux's exception_table) or
// an explicit copy_from_user that walks the page table first. Out of scope
// for this fix — the kernel-pointer-injection hole is what we close here.

#define USER_PATH_MAX 4096

// Returns 1 if [ptr, ptr+len) lies entirely inside the user address range.
// Zero-length buffers are allowed regardless of pointer (matches POSIX).
static inline int user_buf_ok(uint64_t ptr, size_t len) {
  if (len == 0) {
    return 1;
  }
  // Overflow guard first
  if (ptr + len < ptr) {
    return 0;
  }
  if (ptr + len > USER_STACK_TOP) {
    return 0;
  }
  return 1;
}

// Validate a NUL-terminated user string. Returns string length (excluding
// NUL) on success, or -1 if ptr is out of range or no NUL is found within
// USER_PATH_MAX bytes.
static inline int64_t user_str_ok(uint64_t ptr) {
  if (ptr >= USER_STACK_TOP) {
    return -1;
  }
  uint64_t bound = USER_STACK_TOP - ptr;
  if (bound > USER_PATH_MAX) {
    bound = USER_PATH_MAX;
  }
  const char *s = (const char *)ptr;
  for (uint64_t i = 0; i < bound; i++) {
    if (s[i] == '\0') {
      return (int64_t)i;
    }
  }
  return -1; // not NUL-terminated within bound
}

/* SYS_EXEC — replace the calling task's user image with a flat binary read
 * from the VFS at `path`. The new binary is loaded at USER_TEXT_BASE, runs
 * with x0–x30 = 0 and SP_EL0 = USER_STACK_TOP. On success this function
 * does NOT actually return to user space at the SVC site — we modify the
 * trap frame so the eret epilogue lands in the new program's _start.
 *
 * Memory model:
 *   - Code: PMM-allocated pages, RO + EL0-executable, mapped at
 *     USER_TEXT_BASE in a fresh user_l0. Tracked in task->exec_text_phys
 *     so sched_reap (and the next exec) can free them.
 *   - Stack: fresh PMM-allocated pages, RW + UXN, mapped at
 *     [USER_STACK_TOP - 16 KiB, USER_STACK_TOP).
 *   - Old user_l0, old user-stack pages, and any prior exec text are freed
 *     after the TTBR0 swap so we don't yank out from under ourselves.
 *
 * Returns -1 on any failure (open/read/alloc); on success there's no
 * meaningful return because the new image starts running.
 */
#define EXEC_MAX_BYTES (1U << 20) /* 1 MiB cap */
#define EXEC_MAX_ARGC 32          /* hard limit on argv count */
#define EXEC_ARG_BYTES_MAX 1024   /* total string-byte budget for argv */

static int64_t sys_exec(uint64_t arg_path, uint64_t arg_argv,
                        trap_frame_t *frame) {
  if (user_str_ok(arg_path) < 0) {
    return -1;
  }
  const char *path = (const char *)arg_path;

  task_t *cur     = sched_current();
  fd_table_t *fds = cur->fds;
  if (!fds) {
    return -1;
  }

  /* Validate + capture argv while OLD TTBR0 is still active. argv may be
   * NULL (no-args exec), or a NULL-terminated array of NUL-terminated
   * strings. Snapshot to kernel scratch buffers so the post-swap stack
   * can be populated without re-reading user memory.
   *
   * Stack layout we'll build (high → low):
   *   [argv string blob]                 (top)
   *   [16-byte alignment pad]
   *   argv[argc] = NULL
   *   argv[argc-1] = (user VA of str)
   *   ...
   *   argv[0] = (user VA of str)         ← argv pointer (passed in x1)
   *   <SP_EL0 sits 16-byte-aligned below>
   * The program receives argc in x0, argv in x1, envp (NULL) in x2. */
  char    arg_kbuf[EXEC_ARG_BYTES_MAX];
  size_t  arg_offsets[EXEC_MAX_ARGC];
  int     argc      = 0;
  size_t  arg_bytes = 0;

  if (arg_argv != 0) {
    while (argc < EXEC_MAX_ARGC) {
      uint64_t slot_va = arg_argv + (uint64_t)argc * sizeof(uint64_t);
      if (!user_buf_ok(slot_va, sizeof(uint64_t))) {
        return -1;
      }
      const char *p = ((const char **)arg_argv)[argc];
      if (p == NULL) {
        break; /* NULL-terminator slot */
      }
      int64_t len = user_str_ok((uint64_t)p);
      if (len < 0) {
        return -1;
      }
      if (arg_bytes + (size_t)len + 1 > EXEC_ARG_BYTES_MAX) {
        return -1;
      }
      arg_offsets[argc] = arg_bytes;
      memcpy(arg_kbuf + arg_bytes, p, (size_t)len + 1);
      arg_bytes += (size_t)len + 1;
      argc++;
    }
  }


  /* 1. Open and size the binary. */
  int fd = fd_open(fds, path);
  if (fd < 0) {
    return -1;
  }
  int64_t size = fd_seek(fds, fd, 0, SEEK_END);
  fd_seek(fds, fd, 0, SEEK_SET);
  if (size <= 0 || (uint64_t)size > EXEC_MAX_BYTES) {
    fd_close(fds, fd);
    return -1;
  }

  /* 2. Slurp into a kernel buffer. */
  uint8_t *kbuf = (uint8_t *)kmalloc((size_t)size);
  if (!kbuf) {
    fd_close(fds, fd);
    return -1;
  }
  int64_t got = fd_read(fds, fd, kbuf, (size_t)size);
  fd_close(fds, fd);
  if (got != size) {
    kfree(kbuf);
    return -1;
  }

  /* 3. Fresh user stack pages — always 16 KiB, RW + UXN, mapped at the
   * top of the user range. The stack is independent of the binary; the
   * ELF loader handles only PT_LOAD segments. */
  uintptr_t stack_phys = pmm_allocate_pages(USER_STACK_PAGES);
  if (!stack_phys) {
    kfree(kbuf);
    return -1;
  }
  memset((void *)PHYS_TO_VIRT(stack_phys), 0, USER_STACK_PAGES * PAGE_SIZE);

  /* 4. Build a fresh user_l0 and load each PT_LOAD into it. elf_load
   * allocates per-segment PMM pages, copies file bytes, zeros .bss, and
   * maps with permissions derived from each segment's p_flags (RO+X for
   * .text, RW+UXN for .data + .bss, RO+UXN for .rodata). On failure it
   * frees any partial allocations before returning. */
  uint64_t *new_l0 = mmu_create_user_tables();
  if (!new_l0) {
    pmm_free_pages(stack_phys, USER_STACK_PAGES);
    kfree(kbuf);
    return -1;
  }

  elf_image_t img;
  if (elf_load(kbuf, (size_t)size, new_l0, &img) < 0) {
    /* elf_load already freed any per-segment pages it allocated; we still
     * need to tear down the user_l0 page tables and the stack. */
    mmu_free_user_tables(new_l0);
    pmm_free_pages(stack_phys, USER_STACK_PAGES);
    kfree(kbuf);
    return -1;
  }
  kfree(kbuf);

  /* 5. Map the user stack into the new user_l0. */
  uint64_t stack_flags = PTE_ATTRIDX(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
  uint64_t stack_user_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
  mmu_map_user_range(new_l0, stack_user_base, stack_phys, USER_STACK_PAGES,
                     stack_flags);

  /* Print diagnostic *before* the TTBR0 swap. `path` is a user pointer
   * into the OLD address space, which gets unmapped + freed below. */
  uart_printf("[EXEC] Task %d '%s' loading %s (%d bytes, %d region(s), entry %x)\n",
              cur->pid, cur->name, path, (uint64_t)size,
              (uint64_t)img.region_count, img.entry);


  /* 6. Swap in the new image. Save old refs first — we'll free them after
   * TTBR0 has been switched over so we don't yank our own mappings while
   * still using them. */
  uint64_t    old_ttbr0       = cur->ttbr0;
  uintptr_t   old_ustack_phys = cur->ustack_phys;
  elf_image_t old_image       = cur->exec_image;

  /* Allocate a fresh ASID for the new VA layout. With ASIDs every TLB
   * entry is tagged: the new ASID has zero entries cached, so we can
   * safely skip the global tlbi vmalle1 — there's nothing to flush.
   * Stale entries from the old ASID remain in the TLB and will be
   * invalidated explicitly below before the old page tables are freed. */
  uint16_t new_asid  = sched_asid_alloc();
  uint64_t new_ttbr0 = ttbr_pack((uint64_t)new_l0, new_asid);

  cur->ttbr0       = new_ttbr0;
  cur->ustack_phys = stack_phys;
  cur->user_sp     = USER_STACK_TOP;
  cur->exec_image  = img;

  __asm__ __volatile__("msr ttbr0_el1, %0\n\t"
                       "isb"
                       ::"r"(new_ttbr0)
                       : "memory");

  /* 7. Rewrite the trap frame so the syscall epilogue ererts into the new
   * program's _start with a clean register state and the new SP_EL0. */
  for (int i = 0; i < 31; i++) {
    frame->regs[i] = 0;
  }
  frame->elr  = img.entry;
  frame->spsr = 0; /* EL0t, IRQs unmasked */

  /* sp_el0 lives at offset 280 in the 688-byte on-stack trap frame; the
   * C trap_frame_t (sizeof = 280) doesn't expose it. Poke directly. */
  uint64_t *frame_raw = (uint64_t *)frame;
  uint64_t  new_sp    = USER_STACK_TOP;

  if (argc > 0) {
    /* Build argv on the new user stack. Write through the kernel's TTBR1
     * mapping of the freshly-allocated stack pages — PHYS_TO_VIRT(stack_phys)
     * — so we don't depend on TTBR0 already pointing at new_l0. */
    uintptr_t stack_kbase   = PHYS_TO_VIRT(stack_phys);
    uint64_t  stack_user_lo = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);

    /* Strings at the very top, growing downward. */
    uint64_t strings_user_base = USER_STACK_TOP - arg_bytes;
    memcpy((void *)(stack_kbase + (strings_user_base - stack_user_lo)),
           arg_kbuf, arg_bytes);

    /* argv array of (argc + 1) pointers, 16-byte aligned, just below the
     * strings. argv[i] is the *user VA* of the i-th string. */
    uint64_t  argv_user_top  = strings_user_base & ~(uint64_t)15;
    uint64_t  argv_user_base = argv_user_top - (uint64_t)(argc + 1) * 8;
    uint64_t *argv_kernel    = (uint64_t *)(stack_kbase +
                                            (argv_user_base - stack_user_lo));
    for (int i = 0; i < argc; i++) {
      argv_kernel[i] = strings_user_base + arg_offsets[i];
    }
    argv_kernel[argc] = 0;

    /* SP_EL0 sits 16-byte aligned below the argv array. */
    new_sp = argv_user_base & ~(uint64_t)15;

    /* AAPCS64 entry: x0 = argc, x1 = argv, x2 = envp (NULL for now). */
    frame->regs[0] = (uint64_t)argc;
    frame->regs[1] = argv_user_base;
    frame->regs[2] = 0;
  }
  frame_raw[35] = new_sp;



  /* 8. Free old image now that we're no longer using it. mmu_free_user_tables
   * walks intermediate tables only — leaf data pages (text, stack) are
   * freed explicitly above. */
  if (old_ustack_phys) {
    pmm_free_pages(old_ustack_phys, USER_STACK_PAGES);
  }
  /* Free per-segment regions from the previous exec image (if any). */
  for (int i = 0; i < old_image.region_count; i++) {
    if (old_image.regions[i].phys && old_image.regions[i].pages) {
      pmm_free_pages(old_image.regions[i].phys, old_image.regions[i].pages);
    }
  }
  if (old_ttbr0) {
    /* Old user mappings were tagged with the old ASID. Invalidate them
     * before freeing the L1/L2/L3 pages so a future ASID rollover that
     * recycles the old value cannot resurrect entries pointing at pages
     * already returned to the PMM. */
    uint16_t old_asid = ttbr_asid(old_ttbr0);
    if (old_asid) {
      uint64_t arg = (uint64_t)old_asid << TTBR_ASID_SHIFT;
      __asm__ __volatile__("tlbi aside1, %0\n\t"
                           "dsb ish\n\t"
                           "isb"
                           :: "r"(arg));
    }
    mmu_free_user_tables((uint64_t *)ttbr_baddr(old_ttbr0));
  }

/* (diagnostic moved above the swap — `path` and old user state are gone
   here.) */
  return 0;
}


void syscall_dispatch(trap_frame_t *frame) {
  /* Allow preemption during the syscall. EL0 had IRQs unmasked, but the
   * synchronous-from-lower-EL vector entry implicitly masks them. Without
   * this, a long-blocking syscall (e.g. sys_read polling the UART RX
   * register inside the shell) would prevent the timer IRQ from firing
   * and starve every other task. */
  __asm__ __volatile__("msr daifclr, #2" ::: "memory");


  uint64_t num = frame->regs[8];
  uint64_t arg0 = frame->regs[0];
  uint64_t arg1 = frame->regs[1];
  uint64_t arg2 = frame->regs[2];

  int64_t ret = -1;
  fd_table_t *fds = sched_current()->fds;

  switch (num) {
  case SYS_READ:
    if (fds && user_buf_ok(arg1, (size_t)arg2)) {
      ret = fd_read(fds, (int)arg0, (void *)arg1, (size_t)arg2);
    } else {
      uart_errorln("[SYSCALL] SYS_READ rejected: bad user buffer");
    }
    break;

  case SYS_WRITE:
    if (fds && user_buf_ok(arg1, (size_t)arg2)) {
      ret = fd_write(fds, (int)arg0, (const void *)arg1, (size_t)arg2);
    } else {
      uart_errorln("[SYSCALL] SYS_WRITE rejected: bad user buffer");
    }
    break;

  case SYS_OPEN:
    if (fds && user_str_ok(arg0) >= 0) {
      ret = fd_open(fds, (const char *)arg0);
    } else {
      uart_errorln("[SYSCALL] SYS_OPEN rejected: bad user path");
    }
    break;

  case SYS_CLOSE:
    if (fds) {
      ret = fd_close(fds, (int)arg0);
    }
    break;

  case SYS_EXIT:
    task_exit();
    break;

  case SYS_YIELD:
    schedule();
    ret = 0;
    break;

  case SYS_SLEEP:
    sleep_ms(arg0);
    ret = 0;
    break;

  case SYS_GETPID:
    /* Returns the calling task's pid. No arguments, no failure mode. */
    ret = (int64_t)sched_current()->pid;
    break;

  case SYS_LSEEK:
    /* arg0 = fd, arg1 = signed offset, arg2 = whence (SEEK_SET/CUR/END) */
    if (fds) {
      ret = fd_seek(fds, (int)arg0, (int64_t)arg1, (int)arg2);
    }
    break;

  case SYS_UPTIME:
    /* Milliseconds since boot. Uses the kernel's tick counter; resolution
     * is therefore TIMER_INTERVAL_MS (10 ms by default). */
    ret = (int64_t)timer_uptime_ms();
    break;


  case SYS_NET_PING: {
    /* arg0 = seq number. Sends one ICMP echo request to the slirp gateway
     * and busy-polls the RX queue for the matching reply. Returns the
     * reply's IP TTL on success, or -1 if no reply arrived in time.
     *
     * Race: shares net_rx_poll with kernel-mode netd. With single-CPU
     * scheduling and IRQ-driven preemption, both pollers may consume
     * frames the other expected. Both sides handle 'no reply' gracefully
     * so the worst outcome is an occasional spurious failure. */
    uint16_t seq = (uint16_t)arg0;
    if (net_send_ping(seq) <= 0) {
      ret = -1;
      break;
    }
    uint8_t buf[256];
    int got_ttl = -1;
    for (uint32_t spins = 0; spins < 2000000u && got_ttl < 0; spins++) {
      int n = net_rx_poll(buf, sizeof(buf));
      if (n < 14 + 20 + 8)                 continue;
      if (buf[12] != 0x08 || buf[13] != 0x00) continue; /* not IPv4 */
      const uint8_t *ip   = &buf[14];
      const uint8_t *icmp = &buf[14 + 20];
      if (ip[9] != 1 || icmp[0] != 0)      continue;     /* not ICMP echo reply */
      uint16_t reply_seq = ((uint16_t)icmp[6] << 8) | icmp[7];
      if (reply_seq != seq)                 continue;     /* not our reply */
      got_ttl = ip[8];
    }
    ret = (int64_t)got_ttl;
    break;
  }


  case SYS_KILL:
    /* arg0 = pid. Returns 0 on success or -1. */
    ret = (int64_t)sched_kill_task(arg0);
    break;

  case SYS_FORK:
    /* No arguments. Returns child pid to the caller; the child task,
     * when first scheduled, returns 0 from this same SVC via fork_return. */
    ret = (int64_t)sched_fork(sched_current(), frame);
    break;

  case SYS_BALLOON: {
    /* arg0 = op (BALLOON_OP_*), arg1 = page count. Status ops ignore arg1
     * and return the relevant counter. Inflate/deflate return how many
     * pages actually moved. */
    uint32_t actual = 0, target = 0;
    switch (arg0) {
    case BALLOON_OP_INFLATE:
      ret = (int64_t)balloon_inflate((uint32_t)arg1);
      break;
    case BALLOON_OP_DEFLATE:
      ret = (int64_t)balloon_deflate((uint32_t)arg1);
      break;
    case BALLOON_OP_ACTUAL:
      balloon_get_status(&actual, NULL);
      ret = (int64_t)actual;
      break;
    case BALLOON_OP_TARGET:
      balloon_get_status(NULL, &target);
      ret = (int64_t)target;
      break;
    default:
      ret = -1;
      break;
    }
    break;
  }


  case SYS_EXEC: {
    /* arg0 = path, arg1 = argv. On success the trap frame has been fully
     * rewritten so the eret lands in the new program with x0=argc, x1=argv.
     * We MUST NOT fall through to the dispatcher's frame->regs[0] writeback
     * — that would clobber argc. Return early so the eret epilogue uses
     * the frame as-is. On failure (-1), surface that to the caller via the
     * normal x0 return path. */
    int64_t r = sys_exec(arg0, arg1, frame);
    if (r >= 0) {
      return;
    }
    ret = -1;
    break;
  }



  default:
    uart_printf("[SYSCALL] Unknown syscall %u\n", num);
    ret = -1;
    break;
  }
  // write return value back into trap frame
  // x0 is restored with the result on eret
  frame->regs[0] = (uint64_t)ret;
}
