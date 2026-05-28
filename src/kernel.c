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


static inline void sys_sleep(uint64_t ms) {
  register uint64_t x0 __asm__("x0") = ms;
  register uint64_t x8 __asm__("x8") = 6; /* SYS_SLEEP */
  __asm__ __volatile__("svc #0" ::"r"(x0), "r"(x8) : "memory");
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
