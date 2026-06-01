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
#define SYS_GETPID 7
#define SYS_LSEEK 8
#define SYS_UPTIME 9
#define SYS_NET_PING 10  /* arg0 = seq; returns reply TTL or -1 */
#define SYS_KILL 11      /* arg0 = pid; returns 0 or -1 */
#define SYS_FORK 12      /* duplicates the calling task; child sees 0, parent sees child pid */
#define SYS_EXEC 13      /* arg0 = path to flat user binary; replaces caller image */
#define SYS_BALLOON 14   /* arg0 = op (0=inflate,1=deflate,2=actual,3=target), arg1 = n */

/* SYS_BALLOON sub-operations. Inflate/deflate hand pages to/from the host
 * via virtio-balloon; status returns the current size (op=2) or the host's
 * requested target (op=3). */
#define BALLOON_OP_INFLATE 0
#define BALLOON_OP_DEFLATE 1
#define BALLOON_OP_ACTUAL  2
#define BALLOON_OP_TARGET  3

/* SYS_LSEEK whence — mirrors POSIX so user-space and the kernel agree. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2


void syscall_dispatch(trap_frame_t *frame);

#endif
