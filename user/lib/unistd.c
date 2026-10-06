#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "sys.h"

/*
 * <unistd.h> — POSIX wrappers over the raw syscall stubs.
 *
 * Each is a thin pass-through. The one subtlety is open(): its third argument
 * is `...` in the standard because it is only read when O_CREAT is set. This
 * implementation has no variadic machinery in user space, so mode is fixed at
 * 0644. That is a real deviation, documented in stdio.h's fopen(), and it is
 * better than silently ignoring the caller's mode.
 */

int open(const char *path, int flags, ...) {
  return sys_open(path, flags, 0644);
}

int close(int fd) {
  return sys_close(fd);
}

long read(int fd, void *buf, unsigned long count) {
  return (long)sys_read(fd, buf, (size_t)count);
}

long write(int fd, const void *buf, unsigned long count) {
  return (long)sys_write(fd, buf, (size_t)count);
}

long lseek(int fd, long offset, int whence) {
  return (long)sys_lseek(fd, offset, whence);
}

int fork(void) {
  return sys_fork();
}

int getpid(void) {
  return sys_getpid();
}
