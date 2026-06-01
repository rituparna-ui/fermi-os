/*
 * user/cat.c — minimal cat(1) for Fermi OS.
 *
 * Reads each path on argv (or stdin if no args) and writes contents to
 * stdout. Demonstrates the C user-libc model:
 *   - main(int argc, char **argv) entry point
 *   - sys.h inline syscall wrappers
 *   - crt0.o handles _start + SYS_EXIT plumbing
 *
 * Restrictions: this binary lives in a RO + EL0-X mapping, so we can't
 * use mutable globals (.data / .bss isn't yet writable for user images).
 * Only locals + string literals.
 */
#include "sys.h"

static void cat_fd(int fd) {
  char buf[256];
  ssize_t n;
  while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
    sys_write(STDOUT_FILENO, buf, (size_t)n);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    /* No args: cat stdin. /dev/console is line-buffered; this exits when
     * the kernel returns 0 (which it never does for the console — so this
     * branch is mainly for symmetry, real interactive use is rare). */
    cat_fd(STDIN_FILENO);
    return 0;
  }

  for (int i = 1; i < argc; i++) {
    int fd = sys_open(argv[i]);
    if (fd < 0) {
      u_puts("cat: cannot open ");
      u_puts(argv[i]);
      u_puts("\n");
      continue;
    }
    cat_fd(fd);
    sys_close(fd);
  }
  return 0;
}
