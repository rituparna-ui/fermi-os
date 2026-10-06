#include <errno.h>
#include "sys.h"

/*
 * errno support for Fermi libc.
 *
 * The storage itself is in the kernel's per-task TLS page (TPIDR_EL0), so this
 * file only provides the accessor and the error-name table.
 */

int *__errno_location(void) {
  return __fermi_errno_loc();
}

/* strerror: only the codes this kernel actually produces, plus the handful a
 * libc caller is likely to compare against. Anything else reports the generic
 * string rather than returning NULL, which is what POSIX permits and what
 * callers overwhelmingly expect. */
const char *strerror(int err) {
  switch (err) {
  case 0:     return "Success";
  case EPERM: return "Operation not permitted";
  case ENOENT: return "No such file or directory";
  case ESRCH:  return "No such process";
  case EINTR:  return "Interrupted system call";
  case EIO:    return "I/O error";
  case ENXIO:  return "No such device or address";
  case E2BIG:  return "Argument list too long";
  case EBADF:  return "Bad file descriptor";
  case EAGAIN: return "Resource temporarily unavailable";
  case ENOMEM: return "Cannot allocate memory";
  case EACCES: return "Permission denied";
  case EFAULT: return "Bad address";
  case EEXIST: return "File exists";
  case ENODEV: return "No such device";
  case ENOTDIR: return "Not a directory";
  case EISDIR:  return "Is a directory";
  case EINVAL:  return "Invalid argument";
  case ENFILE:  return "Too many open files in system";
  case EMFILE:  return "Too many open files";
  case ENOSPC:  return "No space left on device";
  case ESPIPE:  return "Illegal seek";
  case EROFS:   return "Read-only file system";
  case ERANGE:  return "Numerical result out of range";
  case ENOSYS:  return "Function not implemented";
  case ENOTEMPTY: return "Directory not empty";
  case EOPNOTSUPP: return "Operation not supported";
  default:      return "Unknown error";
  }
}