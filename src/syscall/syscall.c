#include "syscall.h"
#include "mm/mmu/mmu.h" // USER_STACK_TOP
#include "sched/sched.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "vfs/vfs.h"

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

void syscall_dispatch(trap_frame_t *frame) {
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


  default:
    uart_printf("[SYSCALL] Unknown syscall %u\n", num);
    ret = -1;
    break;
  }
  // write return value back into trap frame
  // x0 is restored with the result on eret
  frame->regs[0] = (uint64_t)ret;
}
