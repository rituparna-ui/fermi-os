#ifndef USER_WAIT_H
#define USER_WAIT_H

#include <sys.h>

/*
 * <sys/wait.h> — process termination status.
 *
 * Part of Fermi libc (user/lib/wait.c).
 */

/* waitpid() argument forms. -1 means "any child". */
#define WNOHANG 1 /* return immediately with 0 if no child has exited */
#define WUNTRACED 2
#define WCONTINUED 8

/*
 * Status word predicates, per POSIX. The kernel packs a child's termination
 * into one int and the low 7 bits say which case it is:
 *
 *   normal exit : bits 8-15 = exit code, low 7 bits zero
 *   signalled   : bits 0-6  = signal number, bit 7 (0x80) set
 *
 * A child that dies on a fault is reported as signalled with SIGSEGV (11),
 * which is what it actually was.
 */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define WIFSIGNALED(s) (((s) & 0x7F) != 0)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WCOREDUMP(s)   (((s) & 0x80) != 0)

#define SIGSEGV 11

#define WIFSTOPPED(status)  0

int waitpid(int pid, int *status, int options);
int wait(int *status);

#endif
