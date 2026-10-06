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
#define SYS_WAITPID 260   /* arg0 = pid, arg1 = &status, arg2 = options */

/* Linux waitpid(2) argument forms. */
#define WNOHANG 1  /* defined in sched.h; repeated here for the syscall layer */
#define WEXITED  4

#define SYS_FORK 12      /* duplicates the calling task; child sees 0, parent sees child pid */
#define SYS_EXEC 13      /* arg0 = path to flat user binary; replaces caller image */
#define SYS_BALLOON 14   /* arg0 = op (0=inflate,1=deflate,2=actual,3=target), arg1 = n */

/*
 * mmap family.
 *
 * These deliberately use the LINUX aarch64 syscall numbers (mmap=222,
 * munmap=215, mprotect=226) even though the syscalls above use Fermi's own
 * sequential numbering. Reason: they're new, so there's no back-compatibility
 * to preserve, and pinning them to the real Linux values now means a later
 * switch of the older syscalls to the Linux ABI doesn't move these again.
 *
 * Once the remaining syscalls are renumbered to match Linux, a Linux-ABI libc
 * binary can be exec'd unmodified.
 *
 * mmap arg order follows Linux exactly:
 *   x0 = addr, x1 = length, x2 = prot, x3 = flags, x4 = fd, x5 = offset
 * Six args — the AAPCS64 convention gives us x0-x7, so this fits.
 */
#define SYS_BRK 214      /* arg0 = addr (0 = query current break) */
#define SYS_MUNMAP 215   /* arg0 = addr, arg1 = length */
#define SYS_MMAP 222     /* x0..x5 as above */
#define SYS_MPROTECT 226 /* arg0 = addr, arg1 = length, arg2 = prot */

/* MAP_* flags — identical to Linux values. */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* PROT_* — identical to Linux/POSIX values (see VMA_PROT_* in mmu.h). */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

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

/*
 * errno values — the standard Linux/POSIX numbers, so user space can include
 * <errno.h> and pass them through unchanged. A syscall that fails returns -1
 * AND stores one of these in the calling task's TLS page (readable by user
 * space via TPIDR_EL0), which is the POSIX contract.
 *
 * Only as many as the kernel actually produces are listed. Gaps are
 * intentional rather than filled with guesses.
 */
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define ECHILD  10
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ERANGE  34
#define ENOSYS  38
#define ENOTEMPTY 39
#define EOPNOTSUPP 95


void syscall_dispatch(trap_frame_t *frame);

/* Reset a freshly created task's errno to 0. Called from sched when a task is
 * first entered so it never observes a stale value. */
struct task;
void syscall_reset_errno(struct task *t);

#endif
