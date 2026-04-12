#ifndef SYSCALL_H
#define SYSCALL_H

#include "exception.h"
#include <stdint.h>

// x8 = syscall numbers
#define SYS_WRITE 0
#define SYS_EXIT 1
#define SYS_YIELD 2
#define SYS_SLEEP 3

void syscall_dispatch(trap_frame_t *frame);

#endif
