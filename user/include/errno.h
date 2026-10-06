#ifndef USER_ERRNO_H
#define USER_ERRNO_H

#include "sys.h" /* TPIDR_EL0 accessor + errno value macros */

/*
 * <errno.h> — errno is a macro expanding to (*__fermi_errno_loc()), which
 * dereferences TPIDR_EL0. See sys.h for why it isn't a plain variable.
 */

/* A stable per-task slot. Assigning through the pointer is legal C; reading it
 * multiple times in one expression is not (POSIX leaves the address
 * unspecified, but ours is stable anyway since TPIDR_EL0 doesn't change while
 * the task runs at EL0). */
int *__errno_location(void);

/* Human-readable name for an errno value. Returns "Unknown error" for codes
 * this libc doesn't know. Part of Fermi libc (user/lib/errno.c). */
const char *strerror(int err);

#endif