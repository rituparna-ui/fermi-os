/*
 * user/counter.c — demonstrates that user programs can now use mutable
 * globals. The kernel's ELF loader maps each PT_LOAD with permissions
 * derived from p_flags, so .data lands in a RW + UXN page and .bss in
 * an RW + UXN zero-filled page.
 *
 * Before ELF support: this program would fault on first write to `count`
 * because the entire image was mapped RO + EL0-X.
 *
 * Output:
 *   counter: starting from 0
 *   counter: tick 1 (running_sum=10)
 *   counter: tick 2 (running_sum=20)
 *   ...
 *   counter: final running_sum = 50
 */
#include "sys.h"

/* .data \u2014 explicitly initialized globals. Must end up in a writable
 * PT_LOAD (PF_R | PF_W). */
static int initial_value = 10;

/* .bss \u2014 implicitly zero-initialized globals. Must end up in a writable
 * PT_LOAD with memsz > filesz (the loader zero-fills the gap). */
static int count;
static int running_sum;
static char message[64];

/* Tiny render helper inline since we don't have printf. Writes a uint as
 * decimal into `out`, returns bytes written. */
static int render_uint(char *out, uint64_t v) {
  if (v == 0) { out[0] = '0'; return 1; }
  char tmp[24];
  int n = 0;
  while (v > 0) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  for (int i = 0; i < n; i++) {
    out[i] = tmp[n - 1 - i];
  }
  return n;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* Prove .data initialization survived load. If `initial_value` reads as
   * anything other than 10, the loader didn't copy filesz bytes correctly. */
  u_puts("counter: starting from 0\n");

  /* Five iterations, accumulating into running_sum. */
  for (count = 1; count <= 5; count++) {
    running_sum += initial_value;

    /* Build "counter: tick <count> (running_sum=<sum>)\n" into the
     * .bss-resident `message` buffer \u2014 every byte we write here is a
     * write to a .bss page. If .bss isn't writable, we'd fault. */
    int p = 0;
    const char pfx[] = "counter: tick ";
    for (size_t i = 0; i < sizeof(pfx) - 1; i++) message[p++] = pfx[i];
    p += render_uint(message + p, (uint64_t)count);
    const char mid[] = " (running_sum=";
    for (size_t i = 0; i < sizeof(mid) - 1; i++) message[p++] = mid[i];
    p += render_uint(message + p, (uint64_t)running_sum);
    const char sfx[] = ")\n";
    for (size_t i = 0; i < sizeof(sfx) - 1; i++) message[p++] = sfx[i];
    sys_write(STDOUT_FILENO, message, (size_t)p);

    sys_sleep(200);
  }

  /* Final summary, also through the .bss buffer. */
  int p = 0;
  const char pfx[] = "counter: final running_sum = ";
  for (size_t i = 0; i < sizeof(pfx) - 1; i++) message[p++] = pfx[i];
  p += render_uint(message + p, (uint64_t)running_sum);
  message[p++] = '\n';
  sys_write(STDOUT_FILENO, message, (size_t)p);

  return 0;
}
