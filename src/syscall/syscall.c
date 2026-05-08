#include "syscall.h"
#include "sched/sched.h"
#include "uart/uart.h"
#include "vfs/vfs.h"

// AAPCS64 syscall convention:
//   x8       = syscall number
//   x0 - x7  = up to 7 arguments
//   x0       = return value (written back into trap frame)

void syscall_dispatch(trap_frame_t *frame) {
  uint64_t num = frame->regs[8];
  uint64_t arg0 = frame->regs[0];
  uint64_t arg1 = frame->regs[1];
  uint64_t arg2 = frame->regs[2];

  int64_t ret = -1;
  fd_table_t *fds = sched_current()->fds;

  switch (num) {
  case SYS_READ:
    if (fds) {
      ret = fd_read(fds, (int)arg0, (void *)arg1, (size_t)arg2);
    }
    break;

  case SYS_WRITE:
    if (fds) {
      ret = fd_write(fds, (int)arg0, (const void *)arg1, (size_t)arg2);
    }
    break;

  case SYS_OPEN:
    if (fds) {
      ret = fd_open(fds, (const char *)arg0);
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

  default:
    uart_printf("[SYSCALL] Unknown syscall %u\n", num);
    ret = -1;
    break;
  }
  // write return value back into trap frame
  // x0 is restored with the result on eret
  frame->regs[0] = (uint64_t)ret;
}
