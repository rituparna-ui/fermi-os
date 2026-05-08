#include "blk/blk.h"
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

void enable_fp_simd() {
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

static inline void sys_exit(void) {
  register uint64_t x8 __asm__("x8") = 4; /* SYS_EXIT */
  __asm__ __volatile__("svc #0" ::"r"(x8) : "memory");
}

static inline void sys_sleep(uint64_t ms) {
  register uint64_t x0 __asm__("x0") = ms;
  register uint64_t x8 __asm__("x8") = 6; /* SYS_SLEEP */
  __asm__ __volatile__("svc #0" ::"r"(x0), "r"(x8) : "memory");
}

static void task_a(void) {
  const char msg[] = "[Task A] Hello from EL0 via fd=1!\n";
  sys_write(1, msg, sizeof(msg) - 1);

  const char done[] = "[Task A] exiting\n";
  sys_write(1, done, sizeof(done) - 1);
  sys_exit();
}

static void task_b(void) {
  while (1) {
    const char msg[] = "[Task B] running at EL0\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_sleep(500);
  }
}

// runs in VAS Upper Half after boot.S relocates program counter and stack
// pointer

static int console_write(vnode_t *node, file_t *f, const void *buf,
                         size_t count) {
  (void)node;
  (void)f;
  const char *p = buf;

  for (size_t i = 0; i < count; i++) {
    uart_putc(p[i]);
  }

  return (int)count;
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

  if (fat32_mount() != ESUCCESS) {
    uart_printf("[FS][FAT32] Unable to mount file system");
  }

  vfs_init();

  /* Register /dev/console for UART */
  vnode_t *dev = vfs_create_node(vfs_root(), "dev", VNODE_DIR);
  vnode_t *con = vfs_create_node(dev, "console", VNODE_CHR);

  static file_operations_t console_ops = {
      .read = NULL,
      .write = console_write,
  };
  con->ops = &console_ops;

  /* open fd, write */
  fd_table_t *fdt = fd_table_create();
  int fd = fd_open(fdt, "/dev/console");
  const char *hello = "[VFS] fd_write works testing !\n";
  int n = 0;
  while (hello[n]) {
    n++;
  }
  fd_write(fdt, fd, hello, n);
  fd_close(fdt, fd);
  fd_table_destroy(fdt);
  uint32_t first_cluster, size;

  if (fat32_find("HELLO.TXT", &first_cluster, &size) == ESUCCESS) {
    uart_printf("[FAT32] HELLO.TXT: cluster=%d size=%d\n",
                (uint64_t)first_cluster, (uint64_t)size);

    static char filebuf[512];
    int n = fat32_read(first_cluster, size, filebuf, sizeof(filebuf) - 1);

    if (n > 0) {
      filebuf[n] = 0;
      uart_printf("[FAT32] contents:\n%s\n", filebuf);
    }
  } else {
    uart_errorln("[FAT32] HELLO.TXT not found");
  }

  if (fat32_find("SUBDIR/INFO.TXT", &first_cluster, &size) == ESUCCESS) {
    uart_printf("[FAT32] SUBDIR/INFO.TXT: cluster=%d size=%d\n",
                (uint64_t)first_cluster, (uint64_t)size);

    static char filebuf2[512];
    int n = fat32_read(first_cluster, size, filebuf2, sizeof(filebuf2) - 1);

    if (n > 0) {
      filebuf2[n] = 0;
      uart_printf("[FAT32] contents:\n%s\n", filebuf2);
    }
  } else {
    uart_errorln("[FAT32] SUBDIR/INFO.TXT not found");
  }

  /* Test FAT32 write: create a file, then read it back */
  const char *test_data = "Written by Fermi OS kernel!\n";
  if (fat32_create("CREATED.TXT", test_data, 28) == ESUCCESS) {
    uart_println("[FAT32] Created CREATED.TXT");

    if (fat32_find("CREATED.TXT", &first_cluster, &size) == ESUCCESS) {
      static char vbuf[512];
      int n = fat32_read(first_cluster, size, vbuf, sizeof(vbuf) - 1);
      if (n > 0) {
        vbuf[n] = 0;
        uart_printf("[FAT32] Read back: %s", vbuf);
      }
    } else {
      uart_errorln("[FAT32] CREATED.TXT not found after create");
    }
  } else {
    uart_errorln("[FAT32] Failed to create CREATED.TXT");
  }

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
