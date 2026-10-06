#include <wait.h>
#include "sys.h"

/*
 * <sys/wait.h> implementation.
 *
 * thin pass-throughs to SYS_WAITPID.
 */

int waitpid(int pid, int *status, int options) {
  register long x0 __asm__("x0") = pid;
  register long x1 __asm__("x1") = (long)status;
  register long x2 __asm__("x2") = options;
  register uint64_t x8 __asm__("x8") = 260; /* SYS_WAITPID */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (int)x0;
}

int wait(int *status) {
  return waitpid(-1, status, 0);
}
