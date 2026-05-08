#ifndef SYSCALL_H
#define SYSCALL_H

#include "exception.h"
#include <stdint.h>

// x8 = syscall numbers
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_EXIT 4
#define SYS_YIELD 5
#define SYS_SLEEP 6

void syscall_dispatch(trap_frame_t *frame);

#endif
