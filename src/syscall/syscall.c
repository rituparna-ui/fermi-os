#include "syscall.h"
#include "sched/sched.h"
#include "uart/uart.h"

// AAPCS64-based syscall convention:
//   x8       = syscall number
//   x0 - x7  = up to 7 arguments
//   x0       = return value (written back into trap frame)

void syscall_dispatch(trap_frame_t *frame) {
  uint64_t num = frame->regs[8];  // x8
  uint64_t arg0 = frame->regs[0]; // x0
  uint64_t arg1 = frame->regs[1]; // x1

  switch (num) {
  case SYS_WRITE: {
    // x0 = buffer pointer, x1 = length
    const char *buf = (const char *)arg0;
    uint64_t len = arg1;
    for (uint64_t i = 0; i < len; i++) {
      uart_putc(buf[i]);
    }
    arg0 = len;
    break;
  }

  case SYS_EXIT: {
    uart_printf("[SYSCALL] sys_exit called\n");
    task_exit();
    break;
  }

  case SYS_YIELD: {
    schedule();
    break;
  }

  case SYS_SLEEP: {
    // x0 = milliseconds
    sleep_ms(arg0);
    break;
  }

  default:
    uart_printf("[SYSCALL] Unknown syscall %u\n", num);
    arg0 = (uint64_t)-1;
    break;
  }

  // write return value back into trap frame
  // x0 is restored with the result on eret
  frame->regs[0] = arg0;
}
