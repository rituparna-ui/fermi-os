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

  mmu_init();
  // mmu_run_tests(l1);

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
      "  cat <path>      - print a file\n"
      "  ping            - ICMP echo the slirp gateway (10.0.2.2)\n"
      "  sleep <ms>      - block for <ms> milliseconds\n"
      "  clear           - clear the terminal (ANSI)\n"
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

    } else if (u_streq(line, "clear")) {
      sh_print("\x1b[2J\x1b[H");
    } else if (u_starts_with(line, "sleep ")) {
      int ms = u_atou(line + 6);
      sys_sleep((uint64_t)ms);

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

  heap_init();

  gic_init();

  pci_enumerate_bus();
  pci_virtio_rng_init();
  pci_virtio_blk_init();
  pci_virtio_net_init();

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
