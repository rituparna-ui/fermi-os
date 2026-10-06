#ifndef USER_UNISTD_H
#define USER_UNISTD_H

#include <stddef.h>
#include "sys.h"

/*
 * <unistd.h> — POSIX thin wrappers over the raw syscall stubs.
 *
 * These exist so user code can be written against standard prototypes and later
 * swapped for the real header without touching call sites.
 */

int  open(const char *path, int flags, ...); /* mode is always 0644 here */
int  close(int fd);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long lseek(int fd, long offset, int whence);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

void _exit(int status);

/* fork(): returns 0 in the child, the child's pid in the parent. */
int fork(void);

int getpid(void);

#endif