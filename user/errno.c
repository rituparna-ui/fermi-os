/*
 * errno test — verifies the POSIX two-part error contract from EL0.
 *
 * Every check is observable through the real syscall ABI: the return value in
 * x0 AND the int at TPIDR_EL0. A kernel that returned -1 without setting
 * errno, that shared one errno across tasks, or that cleared errno on success
 * all fail here.
 */
#include "sys.h"

static void say(const char *s) {
  sys_write(STDOUT_FILENO, s, u_strlen(s));
}

static int checks = 0;
static int fails  = 0;

static void expect(int cond, const char *what) {
  checks++;
  if (cond) {
    return;
  }
  fails++;
  say("  [ERRNO TEST] FAIL: ");
  say(what);
  say("\n");
}

int main(void) {
  say("[ERRNO TEST] === errno contract ===\n");

  /* A fresh process starts with errno == 0, never a stale value. */
  expect(errno == 0, "errno starts at 0");

  /* --- 1. a rejected call must set errno, not just return -1 ---
   * File-backed mmap is the cleanest failing call available: deterministic,
   * reachable from EL0, and it touches no memory. */
  errno = 0;
  void *p = sys_mmap(0, 4096, PROT_READ, 0 /* no MAP_ANONYMOUS */, -1, 0);
  expect(p == MAP_FAILED, "file-backed mmap returns MAP_FAILED");
  expect(errno == EOPNOTSUPP, "errno == EOPNOTSUPP for unsupported mapping");

  /* --- 2. bad arguments give EINVAL, not a generic -1 --- */
  errno = 0;
  p = sys_mmap(0, 4096, PROT_READ | 0x40 /* unknown prot bit */,
               MAP_ANONYMOUS, -1, 0);
  expect(p == MAP_FAILED, "unknown prot bit returns MAP_FAILED");
  expect(errno == EINVAL, "errno == EINVAL for unknown prot bits");

  errno = 0;
  int r = sys_munmap((void *)0x123 /* unaligned */, 4096);
  expect(r == -1, "unaligned munmap fails");
  expect(errno == EINVAL, "errno == EINVAL for unaligned munmap");

  errno = 0;
  r = sys_munmap((void *)0x30000000, 0 /* zero length */);
  expect(r == -1, "zero-length munmap fails");
  expect(errno == EINVAL, "errno == EINVAL for zero length");

  /* --- 3. MAP_SHARED is refused with EOPNOTSUPP, never downgraded --- */
  errno = 0;
  p = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED,
               -1, 0);
  expect(p == MAP_FAILED, "MAP_SHARED returns MAP_FAILED");
  expect(errno == EOPNOTSUPP, "errno == EOPNOTSUPP for MAP_SHARED");

  /* --- 4. a SUCCESSFUL call must leave errno alone ---
   * POSIX leaves errno meaningful only after a reported failure. A kernel
   * that zeroed it on success would break code that saves errno, does other
   * work, then inspects it. */
  errno = 0;
  p = sys_mmap(0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  expect(p != MAP_FAILED, "valid mmap succeeds");
  expect(errno == 0, "successful mmap left errno at 0");

  errno = EINVAL; /* plant a value */
  r = sys_munmap(p, 2 * 4096);
  expect(r == 0, "munmap of the region succeeds");
  expect(errno == EINVAL, "successful munmap does NOT clear errno");

  /* --- 5. user space can write errno --- */
  errno = 12345;
  expect(errno == 12345, "user space can write errno");

  /* --- 6. errno is per-task, not one global ---
   * The parent plants a distinctive value, then forks. A single global errno
   * would show the child 4242 and fail the child's check; a correctly
   * per-task one gives the child a fresh 0. The child then writes its own
   * value, which must not disturb the parent's. */
  errno = 4242;
  int child = sys_fork();

  if (child == 0) {
    /* ---- child ---- */
    int cok = 1;
    if (errno != 0) {
      cok = 0;
    }
    errno = 31337;
    if (errno != 31337) {
      cok = 0;
    }
    sys_write(STDOUT_FILENO,
              cok ? "  [ERRNO TEST] child errno independent  OK\n"
                  : "  [ERRNO TEST] child errno independent  FAIL\n",
              cok ? 41 : 43);
    sys_exit();
  }

  expect(child > 0, "fork succeeded");

  /* Let the child run and write its errno before we check ours. */
  sys_sleep(400);

  expect(errno == 4242, "parent errno unaffected by child's writes");

  {
    char buf[24];
    say("[ERRNO TEST] === ");
    sys_write(1, buf, u_utoa(buf, sizeof(buf), (unsigned long)checks));
    say(" checks, ");
    sys_write(1, buf, u_utoa(buf, sizeof(buf), (unsigned long)fails));
    say(" failed ===\n");
  }
  return 0;
}