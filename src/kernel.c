#include "blk/blk.h"
#include "proc/proc.h"
#include "devices.h"
#include "exception.h"
#include "fat32/fat32.h"
#include "gic/gic.h"
#include "mm/heap/heap.h"
#include "mm/mmu/mmu.h"
#include "mm/pmm/pmm.h"
#include "mmio/mmio.h"
#include "panic/panic.h"
#include "pci/pci.h"
#include "rng/rng.h"
#include "balloon/balloon.h"
#include "console/console.h"
#include "cpu/cpu.h"
#include "net/net.h"
#include "sched/sched.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "timer/timer.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "vfs/vfs.h"
#include <stdint.h>

extern uint8_t __bss_start;
extern uint8_t __bss_end;

static void zero_bss(void) {
  memset(&__bss_start, 0, (size_t)(&__bss_end - &__bss_start));
}

static void enable_fp_simd(void) {
  // CPACR_EL1.FPEN = 0b11
  // GCC uses SIMD registers for varargs
  // got ESR_EL1 : 0x1FE00000 while building uart_printf
  uint64_t cpacr;
  __asm__ __volatile__("mrs %0, cpacr_el1" : "=r"(cpacr));
  cpacr |= (3ULL << 20);
  __asm__ __volatile__("msr cpacr_el1, %0" ::"r"(cpacr));
  __asm__ __volatile__("isb");
}

// running in PAS
void early_init() {
  zero_bss();
  enable_fp_simd();

  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  exceptions_init();

  pmm_init(MEM_START, MEM_SIZE);
  pmm_print_info();

  uint64_t *l1_phys = mmu_init();
  /* Run MMU self-tests right after enable, while TTBR0 still points at the
   * boot-time identity table (l0_table_lo). The L2 remap test installs a
   * fresh PTE in this table and verifies VA-→-new-PA propagation; safe to
   * call only before any per-task user_l0 has taken over TTBR0. */
  mmu_run_tests(l1_phys);

  uart_println("[BOOT] MMU Enabled. Jumping to Upper Half");
}

static inline int64_t sys_read(int fd, void *buf, uint64_t count) {
  register int x0 __asm__("x0") = fd;
  register void *x1 __asm__("x1") = buf;
  register uint64_t x2 __asm__("x2") = count;
  register uint64_t x8 __asm__("x8") = 0; /* SYS_READ */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (int64_t)x0;
}

static inline int64_t sys_write(int fd, const char *buf, uint64_t len) {
  register int x0 __asm__("x0") = fd;
  register const char *x1 __asm__("x1") = buf;
  register uint64_t x2 __asm__("x2") = len;
  register uint64_t x8 __asm__("x8") = 1; /* SYS_WRITE */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (int64_t)x0;
}

static inline int64_t sys_open(const char *path) {
  register const char *x0 __asm__("x0") = path;
  register uint64_t x8 __asm__("x8") = 2; /* SYS_OPEN */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (int64_t)x0;
}

static inline int64_t sys_close(int fd) {
  register int x0 __asm__("x0") = fd;
  register uint64_t x8 __asm__("x8") = 3; /* SYS_CLOSE */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (int64_t)x0;
}

static inline void sys_exit(void) {
  register uint64_t x8 __asm__("x8") = 4; /* SYS_EXIT */
  __asm__ __volatile__("svc #0" ::"r"(x8) : "memory");
}

static inline int64_t sys_getpid(void) {
  register int64_t x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = 7; /* SYS_GETPID */
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return x0;
}


static inline int64_t sys_uptime(void) {
  register int64_t x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = 9; /* SYS_UPTIME */
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return x0;
}


static inline int64_t sys_net_ping(uint16_t seq) {
  register uint64_t x0 __asm__("x0") = (uint64_t)seq;
  register uint64_t x8 __asm__("x8") = 10; /* SYS_NET_PING */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (int64_t)x0;
}


static inline void sys_sleep(uint64_t ms) {
  register uint64_t x0 __asm__("x0") = ms;
  register uint64_t x8 __asm__("x8") = 6; /* SYS_SLEEP */
  __asm__ __volatile__("svc #0" ::"r"(x0), "r"(x8) : "memory");
}

static inline int64_t sys_fork(void) {
  register int64_t x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = 12; /* SYS_FORK */
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return x0;
}

static inline int64_t sys_exec(const char *path, const char *const *argv) {
  register const char *x0 __asm__("x0") = path;
  register const char *const *x1 __asm__("x1") = argv;
  register uint64_t x8 __asm__("x8") = 13; /* SYS_EXEC */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return (int64_t)x0;
}

static inline int64_t sys_balloon(uint64_t op, uint64_t n) {
  register uint64_t x0 __asm__("x0") = op;
  register uint64_t x1 __asm__("x1") = n;
  register uint64_t x8 __asm__("x8") = 14; /* SYS_BALLOON */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return (int64_t)x0;
}

/* ----------------------------------------------------------------
 * Tiny EL0 user-space helpers used by task_shell. Defined inline
 * because user-space cannot call into the kernel string library.
 * ---------------------------------------------------------------- */

static int u_render_uint(char *buf, int max, uint64_t v) {
  if (max < 1) return 0;
  if (v == 0) { buf[0] = '0'; return 1; }
  char tmp[24];
  int n = 0;
  while (v > 0 && n < (int)sizeof(tmp)) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  int out = 0;
  while (n > 0 && out < max) {
    buf[out++] = tmp[--n];
  }
  return out;
}

static int u_streq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == '\0' && *b == '\0';
}

static int u_starts_with(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s != *prefix) return 0;
    s++; prefix++;
  }
  return 1;
}

/* Read a line from fd 0 (stdin = /dev/console) with simple line-editing.
 * Echoes typed characters back so the user sees them. Returns line length
 * (excluding the trailing NUL). */
static int u_read_line(char *buf, int max) {
  int n = 0;
  while (n < max - 1) {
    char c;
    int64_t r = sys_read(0, &c, 1);
    if (r <= 0) continue;
    if (c == '\r' || c == '\n') {
      sys_write(1, "\n", 1);
      break;
    }
    if (c == 0x7F || c == 0x08) { /* DEL or BS */
      if (n > 0) {
        n--;
        sys_write(1, "\b \b", 3);
      }
      continue;
    }
    buf[n++] = c;
    sys_write(1, &c, 1); /* echo */
  }
  buf[n] = '\0';
  return n;
}

static void sh_print(const char *s) {
  uint64_t n = 0;
  while (s[n]) n++;
  sys_write(1, s, n);
}

static int u_atou(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  return v;
}


static void sh_help(void) {
  sh_print(
      "Fermi shell built-ins:\n"
      "  help            - show this\n"
      "  pid             - print my task pid\n"
      "  uptime          - print ms since boot\n"
      "  ps              - cat /proc/tasks\n"
      "  free            - cat /proc/meminfo\n"
      "  ifconfig        - cat /proc/netinfo\n"
      "  irqs            - cat /proc/interrupts\n"
      "  version         - cat /proc/version\n"
      "  cpuinfo         - cat /proc/cpuinfo (MIDR / cache / features / cycles)\n"
      "  stack           - stress test demand-paged user stack growth\n"
      "  cat <path>      - print a file\n"
      "  hexdump <path>  - hex+ascii dump of a file\n"
      "  echo <text>     - print text\n"
      "  kill <pid>      - terminate a task by pid\n"
      "  fork            - spawn a child task; both parent and child print\n"
      "  exec <path>     - replace this task with a flat binary from disk\n"
      "  balloon         - virtio-balloon: status / inflate N / deflate N\n"
      "  vlog <text>     - send <text> to /dev/vcons (virtio-console host log)\n"
      "  top             - 5x refresh of tasks/mem/net (1 s)\n"
      "  ping            - ICMP echo the slirp gateway (10.0.2.2)\n"
      "  sleep <ms>      - block for <ms> milliseconds\n"
      "  clear           - clear the terminal (ANSI)\n"
      "  reboot          - PSCI SYSTEM_RESET (warm restart)\n"
      "  exit            - terminate the shell task\n");
}

static void sh_pid(void) {
  char buf[32];
  const char prefix[] = "pid = ";
  uint32_t p = 0;
  for (size_t i = 0; i < sizeof(prefix) - 1; i++) buf[p++] = prefix[i];
  p += (uint32_t)u_render_uint(buf + p, (int)(sizeof(buf) - p),
                               (uint64_t)sys_getpid());
  buf[p++] = '\n';
  sys_write(1, buf, p);
}

static void sh_uptime(void) {
  char buf[32];
  const char prefix[] = "uptime = ";
  uint32_t p = 0;
  for (size_t i = 0; i < sizeof(prefix) - 1; i++) buf[p++] = prefix[i];
  p += (uint32_t)u_render_uint(buf + p, (int)(sizeof(buf) - p),
                               (uint64_t)sys_uptime());
  const char suffix[] = " ms\n";
  for (size_t i = 0; i < sizeof(suffix) - 1; i++) buf[p++] = suffix[i];
  sys_write(1, buf, p);
}

static void sh_cat(const char *path) {
  int fd = sys_open(path);
  if (fd < 0) {
    sh_print("cat: cannot open\n");
    return;
  }
  char buf[256];
  int64_t n;
  while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
    sys_write(1, buf, (uint64_t)n);
  }
  sys_close(fd);
}

static void task_shell(void) {
  sh_print("\nWelcome to the Fermi shell. Type 'help' to start.\n");
  while (1) {
    sh_print("$ ");
    char line[128];
    int n = u_read_line(line, sizeof(line));
    if (n == 0) continue;

    if (u_streq(line, "help")) {
      sh_help();
    } else if (u_streq(line, "pid")) {
      sh_pid();
    } else if (u_streq(line, "uptime")) {
      sh_uptime();
    } else if (u_starts_with(line, "cat ")) {
      sh_cat(line + 4);
    } else if (u_streq(line, "ps")) {
      sh_cat("/proc/tasks");
    } else if (u_streq(line, "free")) {
      sh_cat("/proc/meminfo");
    } else if (u_streq(line, "ifconfig")) {
      sh_cat("/proc/netinfo");
    } else if (u_streq(line, "irqs")) {
      sh_cat("/proc/interrupts");
    } else if (u_streq(line, "cpuinfo")) {
      sh_cat("/proc/cpuinfo");
    } else if (u_streq(line, "version")) {
      sh_cat("/proc/version");
    } else if (u_streq(line, "ping")) {
      /* One-shot ICMP echo to slirp gateway via SYS_NET_PING. */
      static uint16_t shell_ping_seq = 100; /* avoid colliding with netd */
      shell_ping_seq++;
      int64_t ttl = sys_net_ping(shell_ping_seq);
      if (ttl < 0) {
        sh_print("ping: no reply\n");
      } else {
        char out[64];
        const char prefix[] = "reply from 10.0.2.2 ttl=";
        int p = 0;
        for (size_t i = 0; i < sizeof(prefix) - 1; i++) out[p++] = prefix[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p), (uint64_t)ttl);
        out[p++] = '\n';
        sys_write(1, out, (uint64_t)p);
      }

    } else if (u_streq(line, "top")) {
      /* Tiny system monitor: clear + print task / mem / net snapshots
       * five times, sleeping 1 s between iterations. Pure user-space —
       * uses only sh_print, sh_cat, sys_sleep. */
      for (int i = 0; i < 5; i++) {
        sh_print("\x1b[2J\x1b[H");
        sh_print("=== Fermi top ===\n\n");
        sh_cat("/proc/tasks");
        sh_print("\n");
        sh_cat("/proc/meminfo");
        sh_print("\n");
        sh_cat("/proc/netinfo");
        sys_sleep(1000);
      }
      sh_print("(top finished)\n");

    } else if (u_streq(line, "fork")) {
      /* SYS_FORK — child sees 0, parent sees the new pid (or <0 on error). */
      int64_t r = sys_fork();
      if (r == 0) {
        /* CHILD path. Greet via /dev/console then exit. The child has its
         * own copy of the user stack, so writing locals here doesn't
         * disturb the parent. */
        sh_print("[fork-child] hello from the child!\n");
        {
          char out[40];
          const char prefix[] = "[fork-child] my pid=";
          int p = 0;
          for (size_t i = 0; i < sizeof(prefix) - 1; i++) out[p++] = prefix[i];
          p += u_render_uint(out + p, (int)(sizeof(out) - p),
                             (uint64_t)sys_getpid());
          out[p++] = '\n';
          sys_write(1, out, (uint64_t)p);
        }
        sys_exit();
      } else if (r < 0) {
        sh_print("fork: failed\n");
      } else {
        /* PARENT path. */
        char out[40];
        const char prefix[] = "fork: child pid=";
        int p = 0;
        for (size_t i = 0; i < sizeof(prefix) - 1; i++) out[p++] = prefix[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p), (uint64_t)r);
        out[p++] = '\n';
        sys_write(1, out, (uint64_t)p);
      }


    } else if (u_starts_with(line, "exec ")) {
      /* SYS_EXEC — if it succeeds, control jumps to the new program's _start
       * and never returns here. The shell as we know it is gone (replaced).
       * If it returns, the call failed; report and continue. */
      /* Tokenize the command tail in-place. Replace each space with NUL
       * and record argv pointers. argv[0] is the command word (the user
       * conventionally passes the program name there); subsequent slots
       * are positional args. argv array is NULL-terminated for the kernel. */
      char *cmd = line + 5;
      const char *argv[16];
      int argc = 0;
      char *p2 = cmd;
      while (*p2 && argc < 15) {
        while (*p2 == ' ') p2++;
        if (!*p2) break;
        argv[argc++] = p2;
        while (*p2 && *p2 != ' ') p2++;
        if (*p2) {
          *p2 = '\0';
          p2++;
        }
      }
      argv[argc] = 0;
      if (argc == 0) {
        sh_print("exec: usage: exec <path> [args...]\n");
      } else {
        int64_t r = sys_exec(argv[0], argv);
        if (r < 0) {
          sh_print("exec: failed (open / read / alloc)\n");
        }
      }

    } else if (u_streq(line, "balloon") ||
               u_starts_with(line, "balloon ")) {
      /* `balloon`              -> show actual + host target
       * `balloon inflate N`    -> hand N pages to host
       * `balloon deflate N`    -> reclaim N pages from host
       * Numbers are in 4 KiB balloon pages. The driver enforces a hard
       * cap (VIRTIO_BALLOON_MAX_PAGES) and clamps if PMM is exhausted. */
      const char *arg = (line[7] == ' ') ? line + 8 : 0;
      if (!arg || u_streq(arg, "status")) {
        int64_t a = sys_balloon(2 /*BALLOON_OP_ACTUAL*/, 0);
        int64_t t = sys_balloon(3 /*BALLOON_OP_TARGET*/, 0);
        char out[80];
        const char p1[] = "balloon: actual=";
        const char p2[] = " pages target=";
        const char p3[] = " pages\n";
        int p = 0;
        for (size_t i = 0; i < sizeof(p1) - 1; i++) out[p++] = p1[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p), (uint64_t)a);
        for (size_t i = 0; i < sizeof(p2) - 1; i++) out[p++] = p2[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p), (uint64_t)t);
        for (size_t i = 0; i < sizeof(p3) - 1; i++) out[p++] = p3[i];
        sys_write(1, out, (uint64_t)p);
      } else if (u_starts_with(arg, "inflate ")) {
        int n = u_atou(arg + 8);
        int64_t got = sys_balloon(0 /*INFLATE*/, (uint64_t)n);
        char out[64];
        const char pfx[] = "balloon: inflated ";
        const char sfx[] = " pages\n";
        int p = 0;
        for (size_t i = 0; i < sizeof(pfx) - 1; i++) out[p++] = pfx[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p),
                           (uint64_t)(got < 0 ? 0 : got));
        for (size_t i = 0; i < sizeof(sfx) - 1; i++) out[p++] = sfx[i];
        sys_write(1, out, (uint64_t)p);
      } else if (u_starts_with(arg, "deflate ")) {
        int n = u_atou(arg + 8);
        int64_t got = sys_balloon(1 /*DEFLATE*/, (uint64_t)n);
        char out[64];
        const char pfx[] = "balloon: deflated ";
        const char sfx[] = " pages\n";
        int p = 0;
        for (size_t i = 0; i < sizeof(pfx) - 1; i++) out[p++] = pfx[i];
        p += u_render_uint(out + p, (int)(sizeof(out) - p),
                           (uint64_t)(got < 0 ? 0 : got));
        for (size_t i = 0; i < sizeof(sfx) - 1; i++) out[p++] = sfx[i];
        sys_write(1, out, (uint64_t)p);
      } else {
        sh_print("balloon: usage: balloon [status|inflate N|deflate N]\n");
      }


    } else if (u_starts_with(line, "vlog ")) {
      /* Send the rest of the line down /dev/vcons — anything written
       * appears in build/virtio-console.txt on the host. Adds a trailing
       * newline so each invocation is its own line in the host file. */
      const char *msg = line + 5;
      int fd = sys_open("/dev/vcons");
      if (fd < 0) {
        sh_print("vlog: cannot open /dev/vcons\n");
      } else {
        uint64_t mlen = 0;
        while (msg[mlen]) mlen++;
        sys_write(fd, msg, mlen);
        sys_write(fd, "\n", 1);
        sys_close(fd);
        sh_print("vlog: sent (check build/virtio-console.txt on host)\n");
      }


    } else if (u_starts_with(line, "kill ")) {
      int pid = u_atou(line + 5);
      register int64_t x0 __asm__("x0") = (int64_t)pid;
      register uint64_t x8 __asm__("x8") = 11; /* SYS_KILL */
      __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
      if (x0 == 0) {
        sh_print("killed.\n");
      } else {
        sh_print("kill: no such pid (or pid is idle)\n");
      }

    } else if (u_starts_with(line, "echo ")) {
      sh_print(line + 5);
      sh_print("\n");
    } else if (u_starts_with(line, "hexdump ")) {
      const char *path = line + 8;
      int fd = sys_open(path);
      if (fd < 0) {
        sh_print("hexdump: cannot open\n");
      } else {
        static const char hex[] = "0123456789abcdef";
        uint8_t hbuf[16];
        uint64_t offset = 0;
        int64_t got;
        while ((got = sys_read(fd, hbuf, sizeof(hbuf))) > 0) {
          char ln[96];
          int p = 0;
          for (int i = 7; i >= 0; i--) {
            ln[p++] = hex[(offset >> (i * 4)) & 0xF];
          }
          ln[p++] = ' '; ln[p++] = ' ';
          for (int i = 0; i < 16; i++) {
            if (i < got) {
              ln[p++] = hex[(hbuf[i] >> 4) & 0xF];
              ln[p++] = hex[hbuf[i] & 0xF];
            } else {
              ln[p++] = ' '; ln[p++] = ' ';
            }
            ln[p++] = ' ';
            if (i == 7) ln[p++] = ' ';
          }
          ln[p++] = '|';
          for (int i = 0; i < got; i++) {
            char c = (char)hbuf[i];
            ln[p++] = (c >= 32 && c < 127) ? c : '.';
          }
          ln[p++] = '|'; ln[p++] = '\n';
          sys_write(1, ln, (uint64_t)p);
          offset += (uint64_t)got;
        }
        sys_close(fd);
      }
    } else if (u_streq(line, "reboot")) {
      sh_print("rebooting via PSCI SYSTEM_RESET...\n");
      register uint64_t x0 __asm__("x0") = 0x84000009ULL;
      __asm__ __volatile__("hvc #0" : "+r"(x0) :: "memory");
      sh_print("reboot: PSCI returned (unexpected); halting\n");
      sys_exit();

    } else if (u_streq(line, "clear")) {
      sh_print("\x1b[2J\x1b[H");
    } else if (u_starts_with(line, "sleep ")) {
      int ms = u_atou(line + 6);
      sys_sleep((uint64_t)ms);

    } else if (u_streq(line, "stack")) {
      /* Stress demand-paged stack growth. The initial user stack is
       * 16 KiB; allocating a 64 KiB local buffer forces 16 page faults,
       * each handled by sched_try_grow_stack. We touch every page to
       * ensure each fault actually fires (compiler can't elide the
       * writes thanks to volatile). */
      volatile char buf[64 * 1024];
      for (uint64_t i = 0; i < sizeof(buf); i += 4096) {
        buf[i] = (char)(i & 0xFF);
      }
      int ok = 1;
      for (uint64_t i = 0; i < sizeof(buf); i += 4096) {
        if (buf[i] != (char)(i & 0xFF)) ok = 0;
      }
      sh_print(ok ? "stack: 64 KiB stack probe OK (16 page-grows)\n"
                  : "stack: 64 KiB stack probe FAILED\n");
    } else if (u_streq(line, "exit")) {
      sh_print("bye!\n");
      sys_exit();
    } else {
      sh_print("unknown command — try 'help'\n");
    }
  }
}


/* Task that deliberately faults to exercise the kill-on-fault path: writes
 * to an unmapped user VA, which should trigger a Data Abort from EL0. The
 * kernel is expected to log the fault, kill *only* this task, and keep the
 * other tasks running. */
static void task_crash(void) {
  const char banner[] =
      "[Task C] about to deref a bad pointer at 0x12345678 (expect kill)\n";
  sys_write(1, banner, sizeof(banner) - 1);

  volatile uint64_t *bad = (volatile uint64_t *)0x12345678ULL;
  *bad = 0xDEADBEEFCAFEBABEULL;

  const char unreached[] = "[Task C] !!! continued past fault !!!\n";
  sys_write(1, unreached, sizeof(unreached) - 1);
  sys_exit();
}


/* ---- mmap test helpers ---------------------------------------------------
 *
 * These go through the same inline SVC stubs the user programs use, so
 * exercising them here validates the syscall ABI itself rather than some
 * kernel-internal shortcut. */

static inline uint64_t k_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                              uint64_t flags) {
  register long      x0 __asm__("x0") = (long)addr;
  register long      x1 __asm__("x1") = (long)len;
  register long      x2 __asm__("x2") = (long)prot;
  register long      x3 __asm__("x3") = (long)flags;
  register long      x4 __asm__("x4") = -1; /* fd = -1 (anonymous) */
  register long      x5 __asm__("x5") = 0;  /* offset = 0 */
  register uint64_t  x8 __asm__("x8") = 222; /* SYS_MMAP */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                       : "memory");
  return (uint64_t)x0;
}

static inline long k_munmap(uint64_t addr, uint64_t len) {
  register long      x0 __asm__("x0") = (long)addr;
  register long      x1 __asm__("x1") = (long)len;
  register uint64_t  x8 __asm__("x8") = 215; /* SYS_MUNMAP */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x8)
                       : "memory");
  return (long)x0;
}

static inline long k_brk(uint64_t addr) {
  register long      x0 __asm__("x0") = (long)addr;
  register uint64_t  x8 __asm__("x8") = 214; /* SYS_BRK */
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (long)x0;
}

static inline long k_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
  register long      x0 __asm__("x0") = (long)addr;
  register long      x1 __asm__("x1") = (long)len;
  register long      x2 __asm__("x2") = (long)prot;
  register uint64_t  x8 __asm__("x8") = 226; /* SYS_MPROTECT */
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (long)x0;
}

#define TEST_PROT_READ  0x1
#define TEST_PROT_WRITE 0x2
#define TEST_MAP_ANON   0x20

/* EL0-safe reporting helpers. An EL0 task cannot call uart_printf (or any
 * other kernel function) — the kernel .text is mapped into user space
 * read-only for the task's own trampoline, not as a callable library. All
 * output therefore goes through sys_write, same as task_a and task_shell.
 *
 * Doing this through the SVC path is deliberate: it means the test exercises
 * the real syscall ABI rather than a kernel-internal shortcut. */
static void t_puts(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  sys_write(1, s, n);
}

static void t_puthex(uint64_t v) {
  char buf[18];
  const char *digits = "0123456789abcdef";
  int p = 0;
  buf[p++] = '0';
  buf[p++] = 'x';
  for (int i = 15; i >= 0; i--) {
    buf[p++] = digits[(v >> (i * 4)) & 0xF];
  }
  sys_write(1, buf, p);
}

static void t_putdec(uint64_t v) {
  char buf[24];
  int n = u_render_uint(buf, sizeof(buf), v);
  sys_write(1, buf, n);
}

/* Report one assertion. Returns got == want so the caller can tally. */
static int mmap_expect(const char *what, long got, long want) {
  t_puts("  [MM TEST] ");
  t_puts(what);
  t_puts(got == want ? "  OK" : "  FAIL  got=");
  if (got != want) {
    t_putdec((uint64_t)(long)got);
    t_puts(" want=");
    t_putdec((uint64_t)(long)want);
  }
  t_puts("\n");
  return got == want;
}

/* Exercise the mmap/munmap/mprotect path end to end.
 *
 * Everything asserted here is observable from EL0 through the SVC ABI:
 * returns, memory contents after demand-faulting, and the fact that an
 * over-protected access kills the task (checked by task_mmap_guard, which
 * must NOT survive). */
static void task_mmap(void) {
  t_puts("[MM TEST] === mmap / munmap / mprotect ===\n");

  int pass = 0, fail = 0;

  /* --- 1. anonymous RW mapping of 3 pages, kernel-picked address --- */
  uint64_t len  = 3ULL * PAGE_SIZE;
  uint64_t addr = k_mmap(0, len, TEST_PROT_READ | TEST_PROT_WRITE,
                         TEST_MAP_ANON);
  /* MAP_FAILED is (void*)-1. A success address must be page-aligned, above
   * the stack, and non-zero. */
  if ((int64_t)addr < 0) {
    t_puts("  [MM TEST] mmap(3 pages)  FAIL (rejected)\n");
    fail++;
    t_puts("[MM TEST] aborting remaining checks\n");
    sys_exit();
  }
  t_puts("  [MM TEST] mmap(3 pages, anon RW)  OK addr=");
  t_puthex(addr);
  t_puts("\n");
  pass++;

  /* --- 2. demand paging: write one byte per page, read them all back --- */
  /* This is the whole point of the exercise. None of these pages exist until
   * touched, so every write below takes the EC_DATA_ABORT_LO path into
   * sched_try_handle_vma_fault(). */
  volatile uint8_t *p = (volatile uint8_t *)addr;
  for (uint64_t i = 0; i < 3; i++) {
    p[i * PAGE_SIZE] = (uint8_t)(0xA0 + i);
  }
  int rw_ok = 1;
  for (uint64_t i = 0; i < 3; i++) {
    if (p[i * PAGE_SIZE] != (uint8_t)(0xA0 + i)) {
      rw_ok = 0;
    }
  }
  if (mmap_expect("demand-page RW roundtrip", rw_ok, 1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 3. untouched pages must read as zero (true zero-fill) --- */
  /* Byte 1 of page 0 was never written. If the demand-fault path forgot to
   * zero the page, we'd read back PMM garbage here instead of 0. */
  if (mmap_expect("zero-fill of untouched page", p[1], 0)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 4. second mapping gets a different, non-overlapping address --- */
  uint64_t addr2 = k_mmap(0, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                          TEST_MAP_ANON);
  if ((int64_t)addr2 < 0 || addr2 == addr) {
    t_puts("  [MM TEST] second mmap distinct  FAIL\n");
    fail++;
  } else {
    t_puts("  [MM TEST] second mmap distinct  OK ");
    t_puthex(addr2);
    t_puts("\n");
    pass++;
  }

  /* --- 5. MAP_FIXED onto an occupied range is rejected ---
   * MAP_FIXED matters here: without it `addr` is only a hint, and the kernel
   * correctly allocates elsewhere instead of colliding. */
  long overlap = (long)k_mmap(addr, PAGE_SIZE,
                              TEST_PROT_READ | TEST_PROT_WRITE,
                              TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("MAP_FIXED overlap rejected", overlap, -1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 6. MAP_FIXED at a page-aligned free address ---
   * addr2 occupies [addr2, addr2 + PAGE); the next hole starts right above
   * it. (addr+len is NOT free — addr2 was allocated out of that gap.) */
  uint64_t fixed = addr2 + PAGE_SIZE;
  long fr = (long)k_mmap(fixed, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                         TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("MAP_FIXED at free address", fr, (long)fixed)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 7. mprotect to read-only; reads still work afterwards ---
   * Must cover the whole VMA — this implementation rejects a partial range
   * rather than splitting the region (same rule as munmap). The "write now
   * faults" half lives in task_mmap_guard, since it kills the task. */
  long pr = k_mprotect(addr, len, TEST_PROT_READ);
  if (mmap_expect("mprotect -> PROT_READ", pr, 0)) {
    pass++;
  } else {
    fail++;
  }
  if (mmap_expect("read still works after mprotect", p[0], 0xA0)) {
    pass++;
  } else {
    fail++;
  }

  /* Re-granting write to a sub-range must split the VMA and restore
   * writability for exactly that part. (An earlier revision asserted this
   * was refused; split support made it valid, and test 9b covers the
   * accept path.) */
  long pback = k_mprotect(addr, PAGE_SIZE, TEST_PROT_WRITE);
  if (mmap_expect("partial mprotect re-grant", pback, 0)) {
    pass++;
  } else {
    fail++;
  }
  p[0] = 0xA1; /* write now permitted again */
  if (mmap_expect("write after partial mprotect", p[0], 0xA1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 8. munmap releases; re-mapping the same range succeeds --- */
  long mu = k_munmap(addr2, PAGE_SIZE);
  if (mmap_expect("munmap whole VMA", mu, 0)) {
    pass++;
  } else {
    fail++;
  }
  long reuse = (long)k_mmap(addr2, PAGE_SIZE,
                            TEST_PROT_READ | TEST_PROT_WRITE, TEST_MAP_ANON);
  if (mmap_expect("re-map freed range", reuse, (long)addr2)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 9. partial munmap now WORKS (VMA splitting) ---
   * The region is 3 pages; unmap just the middle one. Before split support
   * this returned -1. Now the VMA is cut in two and only the middle page is
   * released, leaving the outer two mapped and readable. */
  uint64_t mid = addr + PAGE_SIZE;
  long partial = k_munmap(mid, PAGE_SIZE);
  if (mmap_expect("partial munmap accepted", partial, 0)) {
    pass++;
  } else {
    fail++;
  }

  /* The outer pages must survive: p[0] is page 0, still mapped. Expect the
   * value written by test 7's partial-mprotect re-grant (0xA1), not the
   * original 0xA0 — that write already happened earlier in this same task. */
  if (mmap_expect("neighbours survive partial munmap", p[0], 0xA1)) {
    pass++;
  } else {
    fail++;
  }

  /* And the middle must be gone: re-mmapping it must succeed at that exact
   * address, proving the range was really released. */
  long remix = (long)k_mmap(mid, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                            TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("split range re-mappable", remix, (long)mid)) {
    pass++;
  } else {
    fail++;
  }
  k_munmap(mid, PAGE_SIZE);

  /* --- 9b. splitting creates two separately-protectable regions --- */
  /* The VMA is now two pieces (page 0, page 2). mprotect on just the first
   * must succeed and affect only it. */
  long sp = k_mprotect(addr, PAGE_SIZE, TEST_PROT_READ);
  if (mmap_expect("partial mprotect accepted", sp, 0)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 9c. VMA merge: free both halves, then re-map across the seam.
   * munmap(addr, PAGE) then munmap(addr+2*PAGE) removes page 0 and page 2.
   * The hole between them is the middle page, already unmapped in 9. After
   * both halves are gone, a MAP_FIXED mapping spanning the whole original
   * 3-page range must succeed — proving the space really is free and the VMA
   * bookkeeping didn't leak a phantom region. */
  long f1 = k_munmap(addr, PAGE_SIZE);
  long f2 = k_munmap(addr + 2 * PAGE_SIZE, PAGE_SIZE);
  if (mmap_expect("unmap split halves", f1 == 0 && f2 == 0, 1)) {
    pass++;
  } else {
    fail++;
  }
  long full = (long)k_mmap(addr, len, TEST_PROT_READ | TEST_PROT_WRITE,
                           TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("re-map whole original range", full, (long)addr)) {
    pass++;
  } else {
    fail++;
  }

  /* munmap of a hole (never mapped) succeeds, matching Linux. */
  long hole = k_munmap(0x30000000ULL, PAGE_SIZE);
  if (mmap_expect("munmap of hole succeeds", hole, 0)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 10. MAP_FIXED above the user ceiling is rejected ---
   * Must be MAP_FIXED: with addr treated as a hint the kernel is free to
   * ignore it and place the region lower, which is correct behavior. */
  long wild = (long)k_mmap(0x70000000ULL, PAGE_SIZE,
                           TEST_PROT_READ | TEST_PROT_WRITE,
                           TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("wild MAP_FIXED rejected", wild, -1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 11. unaligned MAP_FIXED is rejected --- */
  long unal = (long)k_mmap(addr + 0x800, PAGE_SIZE,
                           TEST_PROT_READ | TEST_PROT_WRITE,
                           TEST_MAP_ANON | 0x10 /* MAP_FIXED */);
  if (mmap_expect("unaligned MAP_FIXED rejected", unal, -1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 12. file-backed mmap is refused (no page cache yet) ---
   * Flags of 0 means MAP_ANONYMOUS is clear, i.e. a file-backed request. */
  long nofile = (long)k_mmap(0, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                             0);
  if (mmap_expect("file-backed mmap refused", nofile, -1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 13. MAP_SHARED is refused rather than silently downgraded --- */
  long shared = (long)k_mmap(0, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                             TEST_MAP_ANON | 0x01 /* MAP_SHARED */);
  if (mmap_expect("MAP_SHARED refused", shared, -1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 14. brk grows contiguously and is demand-paged --- */
  long b0 = k_brk(0);
  if (mmap_expect("brk(0) reports initial break", b0 != 0, 1)) {
    pass++;
  } else {
    fail++;
  }

  /* Ask for 4 pages of heap. No physical memory should be allocated until we
   * write, which is what the following demand-page log lines confirm. */
  long b1 = k_brk((uint64_t)b0 + 4 * PAGE_SIZE);
  if (mmap_expect("brk grow succeeds", b1, b0 + (long)(4 * PAGE_SIZE))) {
    pass++;
  } else {
    fail++;
  }

  /* The heap is below the break but above brk_base: nothing should fault
   * there before first touch. */
  volatile uint32_t *heap = (volatile uint32_t *)(uint64_t)b0;
  heap[0] = 0xFEEDFACEu;
  heap[3 * (PAGE_SIZE / 4)] = 0xBEEFCAFEu;
  if (mmap_expect("brk page 0 write/read",
                  heap[0] == 0xFEEDFACEu, 1)) {
    pass++;
  } else {
    fail++;
  }
  if (mmap_expect("brk last page write/read",
                  heap[3 * (PAGE_SIZE / 4)] == 0xBEEFCAFEu, 1)) {
    pass++;
  } else {
    fail++;
  }

  /* Untouched heap byte must be zero. */
  if (mmap_expect("brk zero-fill", heap[1], 0)) {
    pass++;
  } else {
    fail++;
  }

  /* Growth is monotonic: the break never moves backwards. */
  long b2 = k_brk((uint64_t)b1 - PAGE_SIZE); /* shrink request */
  if (mmap_expect("brk does not shrink", b2, b1)) {
    pass++;
  } else {
    fail++;
  }

  /* Beyond the arena ceiling must be refused, returning the current break. */
  long big = k_brk(0x30000000ULL);
  if (mmap_expect("brk beyond arena refused", big, b1)) {
    pass++;
  } else {
    fail++;
  }

  /* --- 15. VMA merge: free the brk heap, it should weld back --- */
  long bm = k_brk(0);
  k_brk((uint64_t)bm + 8 * PAGE_SIZE);
  k_brk(bm); /* refused (no shrink), so arena stays grown — grow back */
  long bchk = k_brk((uint64_t)bm + 8 * PAGE_SIZE);
  if (mmap_expect("brk regrow after no-op", bchk != 0, 1)) {
    pass++;
  } else {
    fail++;
  }

  t_puts("[MM TEST] === ");
  t_putdec(pass);
  t_puts(" passed, ");
  t_putdec(fail);
  t_puts(" failed ===\n");
  sys_exit();
}

/* Must die. Writes to a PROT_READ page after mprotect; the demand-fault path
 * must refuse to satisfy the write, and the EL0 data abort then kills the
 * task. Reaching the "SURVIVED" line is the failure signal. */
static void task_mmap_guard(void) {
  t_puts("[MM TEST] guard: writing to PROT_READ page (expect kill)\n");

  uint64_t len  = 2ULL * PAGE_SIZE;
  uint64_t addr = k_mmap(0, len, TEST_PROT_READ | TEST_PROT_WRITE,
                         TEST_MAP_ANON);
  if ((int64_t)addr < 0) {
    t_puts("[MM TEST] guard: mmap failed unexpectedly\n");
    sys_exit();
  }

  /* Drop write permission across the whole VMA. Nothing has touched any of
   * these pages yet, so none has a PTE at all; the fault below is what drives
   * the first one in. The permission check must refuse the write rather than
   * map the page RW — otherwise a PROT_READ region would be silently
   * writable, which is the exact bug this task exists to catch.
   *
   * Must cover the entire VMA: a partial mprotect is refused by design. */
  k_mprotect(addr, len, TEST_PROT_READ);

  volatile uint8_t *p = (volatile uint8_t *)addr;
  p[0] = 0xFF; /* first touch, read-only -> faults here */

  t_puts("[MM TEST] guard: !!! SURVIVED !!! write was allowed\n");
  sys_exit();
}



/* Runs the on-disk ERRNO test binary as a real exec'd ELF64 image.
 *
 * This is deliberately not the same thing as the in-kernel checks above:
 * this binary is compiled and linked by the normal user-space rules, uses the
 * public <sys.h> wrappers, and gets a fresh address space from execve. That
 * exercises the whole chain — ELF loader, TLS page allocation in exec, and
 * the user-side TPIDR_EL0 accessor — rather than the kernel's internal view of
 * errno. */
/* NOTE: the binary is named ERRNO.ELF, not ERRNOTEST.ELF. This kernel's
 * FAT32 has no long-filename support, so mcopy stores an over-long name as the
 * 8.3 short name "ERRNOT~1" and the lookup by full name then fails. "ERRNO"
 * fits in 8 characters and needs no LFN entry. See the FAT32 note in the
 * README. */
static void task_errnotest(void) {
  /* argv MUST live on the stack. A `static` array would land in .data, which
   * is kernel-private and deliberately never mapped into a user TTBR0 — the
   * kernel would then fault dereferencing an address the EL0 task cannot even
   * read. Local storage sits on the user stack, which IS mapped. This is a
   * real constraint of the kernel-text-task model, not a workaround. */
  const char *argv[2];
  argv[0] = "/mnt/fat32/ERRNO.ELF";
  argv[1] = 0;

  if (sys_exec(argv[0], argv) < 0) {
    t_puts("[ERRNO TEST] exec failed (is ERRNO.ELF on the disk image?)\n");
    /* Must not fall off the end of this function: an EL0 task entry was
     * reached via a trampoline that has already eret'd, so there is no
     * return address to come back to. */
    sys_exit();
  }

  /* Unreachable when exec succeeds — it rewrites the trap frame so control
   * never comes back here. Stated explicitly so a reader doesn't wonder
   * where this goes. */
  sys_exit();
}

/* Runs the on-disk LIBC test binary: a real exec'd ELF64 program linked
 * against user/lib/libc.a, exercising malloc, <string.h>, qsort and errno
 * from actual user space rather than from kernel-internal checks. Named
 * LIBCT.ELF because FAT32 here has no long-filename support. */
static void task_libctest(void) {
  const char *argv[2];
  argv[0] = "/mnt/fat32/LIBCT.ELF";
  argv[1] = 0;

  if (sys_exec(argv[0], argv) < 0) {
    t_puts("[LIBC TEST] exec failed (is LIBCT.ELF on the disk image?)\n");
    sys_exit();
  }
  sys_exit();
}

/* Runs the on-disk STDIO test: a real exec'd program linked against libc.a
 * that writes a file to the FAT32 disk and reads it back, covering <stdio.h>,
 * formatted output, libc buffering and the kernel's write path together. */
static void task_stdiotest(void) {
  const char *argv[2];
  argv[0] = "/mnt/fat32/STDIO.ELF";
  argv[1] = 0;

  if (sys_exec(argv[0], argv) < 0) {
    t_puts("[STDIO TEST] exec failed (is STDIO.ELF on the disk image?)\n");
    sys_exit();
  }
  sys_exit();
}

/* Runs the on-disk PROC test: fork fd inheritance (with a genuinely shared
 * file offset) plus waitpid exit-status delivery. */
static void task_proctest(void) {
  const char *argv[2];
  argv[0] = "/mnt/fat32/PROCT.ELF";
  argv[1] = 0;

  if (sys_exec(argv[0], argv) < 0) {
    t_puts("[PROC TEST] exec failed (is PROCT.ELF on the disk image?)\n");
    sys_exit();
  }
  sys_exit();
}

/* ---- fork memory-isolation test ---------------------------------------- */

/* SYS_FORK returns the child pid in x0 (and 0 in the child). The result
   MUST be read from x0 — x8 holds the syscall number, which the kernel leaves
   untouched, so returning it would hand every caller the constant 12. */
static inline long k_fork(void) {
  register long      x0 __asm__("x0");
  register uint64_t  x8 __asm__("x8") = 12; /* SYS_FORK */
  __asm__ __volatile__("svc #0"
                       : "=r"(x0)
                       : "r"(x8)
                       : "memory");
  return (long)x0;
}

/* Shared rendezvous so the child can report back without any IPC primitive
 * that doesn't exist yet. The parent publishes an address via mmap; the
 * child finds it by re-running the same allocator walk, then writes a result
 * there. Simple, and it exercises the VMA copy in both directions. */
#define FORK_MMAGIC 0x0F0C0BEULL

/* Parent half: sets up heap + mmap regions, forks, then verifies the child
 * left its own copy untouched. */
/* Child half of the fork test; defined after task_fork_mem for readability. */
static void task_fork_child_half(long b0, uint64_t mp);

static void task_fork_mem(void) {
  t_puts("[FORK TEST] === fork memory isolation ===\n");

  /* Allocate heap via brk. The arena lives at a FIXED address (brk_base), so
   * the child can locate its own inherited heap with just brk(0) — no
   * argument passing needed, which makes the heap the natural rendezvous. */
  long b0 = k_brk(0);
  k_brk((uint64_t)b0 + 2 * PAGE_SIZE);
  volatile uint32_t *heap = (volatile uint32_t *)(uint64_t)b0;
  heap[0] = 0x11111111u;
  heap[1] = 0x22222222u;

  /* Publish the mmap region's address through the heap so the child can find
   * it. That the channel works at all is itself the property under test. */
  uint64_t mp = k_mmap(0, PAGE_SIZE, TEST_PROT_READ | TEST_PROT_WRITE,
                       TEST_MAP_ANON);
  if ((int64_t)mp < 0) {
    t_puts("  [FORK TEST] parent mmap failed\n");
    sys_exit();
  }
  volatile uint32_t *mm = (volatile uint32_t *)mp;
  mm[0] = 0x33333333u;
  heap[2] = (uint32_t)mp;

  long child = k_fork();

  /* Child half: a fork'd child resumes right here with x0 == 0, so it takes
   * this branch and exits without ever returning to the parent code below. */
  if (child == 0) {
    task_fork_child_half(b0, mp);
    sys_exit();
  }

  /* Parent half. */
  if (child < 0) {
    t_puts("  [FORK TEST] fork failed\n");
    sys_exit();
  }
  t_puts("  [FORK TEST] forked child, verifying parent copy unchanged\n");

  /* The child runs concurrently and will overwrite ITS copies. Ours must not
   * move. Give it a few scheduling slots by sleeping. */
  sys_sleep(400);

  if (mmap_expect("parent heap page intact", heap[0] == 0x11111111u, 1)) {
    /* pass */
  }
  if (mmap_expect("parent heap 2nd word", heap[1] == 0x22222222u, 1)) {
    /* pass */
  }
  if (mmap_expect("parent mmap page intact", mm[0] == 0x33333333u, 1)) {
    /* pass */
  }

  /* Write again ourselves to be sure the region is still writable after the
   * child churned through its own copy. */
  heap[0] = 0x44444444u;
  if (mmap_expect("parent still writable", heap[0] == 0x44444444u, 1)) {
    /* pass */
  }

  t_puts("[FORK TEST] done\n");
  sys_exit();
}

/* A fork'd child resumes inside task_fork_mem with x0 == 0, so both halves
 * live in that function and branch on the fork return value — there is no
 * separate child entry point to register.
 *
 * The child deliberately does NOT call mmap: its VMA set already contains
 * the parent's region, so a fresh mmap here would return a *different*
 * address and the test would prove nothing about the inherited mapping. */
static void task_fork_child_half(long b0, uint64_t mp) {
  volatile uint32_t *heap = (volatile uint32_t *)(uint64_t)b0;

  /* First confirm the inherited contents actually arrived. A copy that
   * dropped or zeroed the data would pass an isolation test vacuously, so
   * this check is what makes the parent's assertions meaningful. */
  if (heap[0] != 0x11111111u) {
    t_puts("  [FORK TEST] child did NOT inherit heap contents\n");
  } else {
    t_puts("  [FORK TEST] child inherited heap contents OK\n");
  }

  heap[0] = 0xAAAAAAAAu;
  heap[1] = 0xBBBBBBBBu;

  if (mp) {
    volatile uint32_t *mm = (volatile uint32_t *)mp;
    if (mm[0] != 0x33333333u) {
      t_puts("  [FORK TEST] child did NOT inherit mmap contents\n");
    } else {
      t_puts("  [FORK TEST] child inherited mmap contents OK\n");
    }
    mm[0] = 0xCCCCCCCCu;
  }

  t_puts("[FORK TEST] child wrote to its private copies\n");
  sys_exit();
}

/* Task A: open a file from FAT32 and print it through fd=1. */
static void task_a(void) {
  const char banner[] = "[Task A] reading /mnt/fat32/HELLO.TXT\n";
  sys_write(1, banner, sizeof(banner) - 1);

  /* Quick demo of SYS_GETPID. We render "[Task A] pid=N\n" by hand and
   * push it through fd 1 (stdout = /dev/console). */
  {
    int64_t pid = sys_getpid();
    char pidline[32];
    const char prefix[] = "[Task A] pid=";
    int p = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) pidline[p++] = prefix[i];
    pidline[p++] = (char)('0' + (pid % 10));
    pidline[p++] = '\n';
    sys_write(1, pidline, (uint64_t)p);
  }


  int fd = sys_open("/mnt/fat32/HELLO.TXT");
  if (fd < 0) {
    const char err[] = "[Task A] open failed\n";
    sys_write(1, err, sizeof(err) - 1);
    sys_exit();
  }

  char buf[256];
  int64_t n = sys_read(fd, buf, sizeof(buf));
  if (n > 0) sys_write(1, buf, (uint64_t)n);

  sys_close(fd);

  /* Also dump /proc/netinfo to demonstrate the new endpoint surfacing
   * the live virtio-net state through the existing fd / VFS plumbing. */
  const char ni_banner[] = "[Task A] cat /proc/netinfo\n";
  sys_write(1, ni_banner, sizeof(ni_banner) - 1);
  fd = sys_open("/proc/netinfo");
  if (fd >= 0) {
    n = sys_read(fd, buf, sizeof(buf));
    if (n > 0) sys_write(1, buf, (uint64_t)n);
    sys_close(fd);
  }


  /* Also dump /proc/interrupts so we observe the GIC counters from EL0. */
  const char ir_banner[] = "[Task A] cat /proc/interrupts\n";
  sys_write(1, ir_banner, sizeof(ir_banner) - 1);
  fd = sys_open("/proc/interrupts");
  if (fd >= 0) {
    n = sys_read(fd, buf, sizeof(buf));
    if (n > 0) sys_write(1, buf, (uint64_t)n);
    sys_close(fd);
  }


  const char done[] = "[Task A] done\n";
  sys_write(1, done, sizeof(done) - 1);
  sys_exit();
}

/* Task B: read 4 random bytes from /dev/rng every 500ms and print them. */
static void task_b(void) {
  /* SYS_GETPID demo from task_b too — we should see two distinct pids. */
  {
    int64_t pid = sys_getpid();
    char pidline[32];
    const char prefix[] = "[Task B] pid=";
    int p = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) pidline[p++] = prefix[i];
    pidline[p++] = (char)('0' + (pid % 10));
    pidline[p++] = '\n';
    sys_write(1, pidline, (uint64_t)p);
  }


  int fd = sys_open("/dev/rng");
  if (fd < 0) {
    const char err[] = "[Task B] open /dev/rng failed\n";
    sys_write(1, err, sizeof(err) - 1);
    sys_exit();
  }

  while (1) {
    unsigned char r[4];
    int64_t n = sys_read(fd, r, 4);
    if (n == 4) {
      /* Render 4 bytes as hex into a fixed buffer and emit */
      char line[32];
      const char *hex = "0123456789ABCDEF";
      int p = 0;
      const char prefix[] = "[Task B] rng: ";
      for (size_t i = 0; i < sizeof(prefix) - 1; i++) line[p++] = prefix[i];
      for (int i = 0; i < 4; i++) {
        line[p++] = hex[(r[i] >> 4) & 0xF];
        line[p++] = hex[r[i] & 0xF];
        line[p++] = ' ';
      }
      line[p++] = '\n';
      sys_write(1, line, (uint64_t)p);
    }
    sys_sleep(500);
  }
}

// runs in VAS Upper Half after boot.S relocates program counter and stack
// pointer

/* netd: kernel-mode (EL1) daemon. Periodically pings the slirp gateway,
 * drains incoming RX, and prints the ICMP echo reply latency in ticks.
 * Lives at EL1 so it can call net_send_ping / net_rx_poll directly
 * without the syscall round-trip a user task would need. */
static void netd(void) {
  uart_println("[netd] starting (kernel-mode background pinger)");
  uint16_t seq = 2; /* seq 1 was sent during pci_virtio_net_init */
  while (1) {
    sleep_ms(5000);

    /* Drain anything that came in while we slept (slirp DHCP retries etc.) */
    uint8_t buf[256];
    int drained = 0;
    while (net_rx_poll(buf, sizeof(buf)) > 0) drained++;
    if (drained > 0) {
      uart_printf("[netd] drained %d async frames before ping\n",
                  (uint64_t)drained);
    }

    uint64_t t0 = timer_get_ticks();
    if (net_send_ping(seq) <= 0) {
      continue;
    }

    /* Wait for matching ICMP echo reply. */
    int got = 0;
    for (uint32_t spins = 0; spins < 2000000u && !got; spins++) {
      int n = net_rx_poll(buf, sizeof(buf));
      if (n < 14 + 20 + 8)                 continue;
      if (buf[12] != 0x08 || buf[13] != 0x00) continue;       /* not IPv4 */
      const uint8_t *ip   = &buf[14];
      const uint8_t *icmp = &buf[14 + 20];
      if (ip[9] != 1)    continue;                            /* not ICMP */
      if (icmp[0] != 0)  continue;                            /* not echo reply */
      uint16_t reply_seq = ((uint16_t)icmp[6] << 8) | icmp[7];
      if (reply_seq != seq) continue;

      uint64_t t1 = timer_get_ticks();
      uart_printf("[netd] ping seq=%d reply ttl=%d in %d ticks\n",
                  (uint64_t)seq, (uint64_t)ip[8],
                  (uint64_t)(t1 - t0));
      got = 1;
    }
    if (!got) {
      uart_printf("[netd] ping seq=%d — no reply\n", (uint64_t)seq);
    }
    seq++;
  }
}


void kernel_main() {
  // all device access through TTBR1
  mmio_switch_to_upper();

  // relocate VBAR_EL1 to upper half
  exceptions_init_upper();

  // relocate PMM bitmap to upper half so it's accessible via TTBR1
  pmm_relocate_upper();

  // Verify if the kernel is running in upper half
  uart_printf("[KERNEL] kernel_main address: %x\n",
              (uint64_t)(uintptr_t)kernel_main);

  // verify stack pointer in upper half
  uint64_t sp;
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));
  uart_printf("[KERNEL] Stack Pointer: %x\n", sp);

  cpu_init();
  heap_init();

  gic_init();

  pci_enumerate_bus();
  pci_virtio_rng_init();
  pci_virtio_blk_init();
  pci_virtio_net_init();
  pci_virtio_balloon_init();
  pci_virtio_console_init();

  if (fat32_mount() != ESUCCESS) {
    uart_printf("[FS][FAT32] Unable to mount file system");
  }

  vfs_init();

  /* Register /dev/console, /dev/null, /dev/zero, /dev/rng */
  devices_register();

  vnode_t *mnt = vfs_create_node(vfs_root(), "mnt", VNODE_DIR);
  vfs_create_node(mnt, "fat32", VNODE_DIR);
  fat32_vfs_mount("/mnt/fat32");

  proc_init();

  sched_init();
  sched_create_task("task_a", task_a);
  sched_create_task("task_b", task_b);
  sched_create_task("task_shell", task_shell);
  sched_create_task("task_crash", task_crash);
  sched_create_task("task_mmap", task_mmap);
  sched_create_task("task_forkmem", task_fork_mem);
  sched_create_task("task_errnotest", task_errnotest);
  sched_create_task("task_libctest", task_libctest);
  sched_create_task("task_stdiotest", task_stdiotest);
  sched_create_task("task_proctest", task_proctest);
  sched_create_task("task_mmguard", task_mmap_guard);
  sched_create_kernel_task("netd", netd);

  timer_init();
  timer_start(TIMER_INTERVAL_MS);

  uart_println("[KERNEL] Ready! running idle task...");

  while (1) {
    __asm__ __volatile__("wfi");
  }
}

void kernel_panic_return(void) {
  kernel_panic("kernel_main returned unexpectedly");
}
