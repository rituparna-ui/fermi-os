#include "proc.h"
#include "net/net.h"
#include "gic/gic.h"
#include "mm/heap/heap.h"
#include "mm/pmm/pmm.h"
#include "sched/sched.h"
#include "strings/strings.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "vfs/vfs.h"
#include "balloon/balloon.h"
#include "cpu/cpu.h"

/* Each /proc file regenerates its content on every read out of live
 * kernel state. The caller-provided buf is filled with bytes
 * [f->offset, f->offset + count); the file appears to grow until the
 * generator finishes, then the caller sees EOF (return 0).
 *
 * Generators write into a stack-local buffer (PROC_BUF_BYTES) and are
 * expected to fit their snapshot in there. If a snapshot ever exceeds
 * the buffer, kvsnprintf truncates rather than overflowing — the file
 * just appears shorter than it should.
 */
#define PROC_BUF_BYTES 2048

typedef int (*proc_generator_fn)(char *buf, size_t buflen);

/* Common read implementation: run a generator into a temp buffer,
 * then memcpy out [offset, offset+count). */
static int proc_read_via(proc_generator_fn gen, file_t *f, void *buf,
                         size_t count) {
  char tmp[PROC_BUF_BYTES];
  int total = gen(tmp, sizeof(tmp));
  if (total < 0) {
    return -1;
  }
  /* kvsnprintf returns the number of bytes that *would* have been written;
   * cap at buffer capacity for the actual readable size. */
  size_t avail = (size_t)total;
  if (avail >= sizeof(tmp)) {
    avail = sizeof(tmp) - 1;
  }
  if ((uint64_t)f->offset >= avail) {
    return 0; /* EOF */
  }
  size_t remaining = avail - (size_t)f->offset;
  size_t to_copy = (count < remaining) ? count : remaining;
  memcpy(buf, tmp + f->offset, to_copy);
  f->offset += to_copy;
  return (int)to_copy;
}

/* ------------------------------------------------------------------ */
/* Generators                                                          */
/* ------------------------------------------------------------------ */

static int gen_uptime(char *buf, size_t buflen) {
  uint64_t ms = timer_uptime_ms();
  uint64_t s = ms / 1000;
  uint64_t cs = (ms % 1000) / 10; /* hundredths of a second */
  return ksnprintf(buf, buflen, "%u.%u%u\n", s, cs / 10, cs % 10);
}

static int gen_meminfo(char *buf, size_t buflen) {
  uint64_t total_pages = pmm_get_total_pages();
  uint64_t used_pages = pmm_get_used_pages();
  uint64_t free_pages = pmm_get_free_pages();
  uint64_t reserved = pmm_get_reserved_pages();

  uint64_t heap_used = heap_used_bytes();
  uint64_t heap_free = heap_free_bytes();
  uint64_t heap_total = heap_total_bytes();

  return ksnprintf(buf, buflen,
                   "MemTotal:    %u KB\n"
                   "MemUsed:     %u KB\n"
                   "MemFree:     %u KB\n"
                   "MemReserved: %u KB\n"
                   "HeapTotal:   %u KB\n"
                   "HeapUsed:    %u KB\n"
                   "HeapFree:    %u KB\n",
                   (total_pages * 4),     /* 4 KiB pages */
                   (used_pages * 4),
                   (free_pages * 4),
                   (reserved * 4),
                   (heap_total / 1024),
                   (heap_used / 1024),
                   (heap_free / 1024));
}

static int gen_tasks(char *buf, size_t buflen) {
  size_t pos = 0;

  int n = ksnprintf(buf, buflen,
                    "PID  STATE     NAME\n"
                    "---- --------- ----------------\n");
  pos += (n < 0) ? 0 : (size_t)n;

  task_t *head = sched_first_task();
  task_t *t = head;
  do {
    if (pos >= buflen) {
      break;
    }
    int wrote =
        ksnprintf(buf + pos, buflen - pos, "%u  %s   %s\n",
                  (uint64_t)t->pid, task_state_name(t->state), t->name);
    if (wrote > 0) {
      pos += (size_t)wrote;
    }
    t = t->next;
  } while (t && t != head);

  return (int)pos;
}

static int gen_netinfo(char *buf, size_t buflen) {
  /* net.c builds the snapshot directly into the caller buffer. The
   * proc_read_via wrapper handles offset / EOF semantics around it. */
  return net_get_info(buf, (uint32_t)buflen);
}

static int gen_interrupts(char *buf, size_t buflen) {
  /* gic_render_interrupts writes directly into our buffer; the
   * proc_read_via wrapper handles offset / EOF semantics around it. */
  return gic_render_interrupts(buf, (uint32_t)buflen);
}


static int gen_cmdline(char *buf, size_t buflen) {
  /* Stub kernel command line. Once we add real boot-arg parsing this will
   * surface what was actually passed; for now report the static defaults
   * the kernel comes up with. */
  return ksnprintf(buf, buflen,
                   "console=ttyAMA0 maxcpus=1 net=virtio-net-pci ip=10.0.2.15\n");
}


static int gen_version(char *buf, size_t buflen) {
  return ksnprintf(buf, buflen,
                   "Fermi OS aarch64 (cortex-a72)\n"
                   "Built: " __DATE__ " " __TIME__ "\n");
}

static int gen_balloon(char *buf, size_t buflen) {
  /* Both counters are in 4 KiB balloon pages. KB == pages * 4. */
  uint32_t actual = 0, target = 0;
  balloon_get_status(&actual, &target);
  return ksnprintf(buf, buflen,
                   "actual:      %u pages (%u KB)\n"
                   "host_target: %u pages (%u KB)\n",
                   (uint64_t)actual, (uint64_t)actual * 4,
                   (uint64_t)target, (uint64_t)target * 4);
}

static int gen_cpuinfo(char *buf, size_t buflen) {
  return cpu_render_info(buf, buflen);
}



/* ------------------------------------------------------------------ */
/* file_operations wrappers                                            */
/* ------------------------------------------------------------------ */

static int read_uptime(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_uptime, f, buf, count);
}
static int read_meminfo(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_meminfo, f, buf, count);
}
static int read_tasks(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_tasks, f, buf, count);
}
static int read_netinfo(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_netinfo, f, buf, count);
}

static int read_interrupts(struct vnode *n, file_t *f, void *buf,
                           size_t count) {
  (void)n;
  return proc_read_via(gen_interrupts, f, buf, count);
}

static int read_cmdline(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_cmdline, f, buf, count);
}

static int read_balloon(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_balloon, f, buf, count);
}


static int read_cpuinfo(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_cpuinfo, f, buf, count);
}


static int read_version(struct vnode *n, file_t *f, void *buf, size_t count) {
  (void)n;
  return proc_read_via(gen_version, f, buf, count);
}

static file_operations_t uptime_ops  = {.read = read_uptime,  .write = 0};
static file_operations_t meminfo_ops = {.read = read_meminfo, .write = 0};
static file_operations_t tasks_ops   = {.read = read_tasks,   .write = 0};
static file_operations_t netinfo_ops = {.read = read_netinfo, .write = 0};
static file_operations_t interrupts_ops = {.read = read_interrupts, .write = 0};

static file_operations_t cmdline_ops = {.read = read_cmdline, .write = 0};

static file_operations_t balloon_ops = {.read = read_balloon, .write = 0};


static file_operations_t cpuinfo_ops = {.read = read_cpuinfo, .write = 0};


static file_operations_t version_ops = {.read = read_version, .write = 0};

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static void register_file(vnode_t *parent, const char *name,
                          file_operations_t *ops) {
  vnode_t *n = vfs_create_node(parent, name, VNODE_REG);
  if (!n) {
    uart_printf("[PROC] Failed to create /proc/%s\n", name);
    return;
  }
  n->ops = ops;
}

void proc_init(void) {
  uart_println("[PROC] Initializing /proc");

  vnode_t *proc = vfs_create_node(vfs_root(), "proc", VNODE_DIR);
  if (!proc) {
    uart_errorln("[PROC] Failed to create /proc");
    return;
  }

  register_file(proc, "uptime",  &uptime_ops);
  register_file(proc, "meminfo", &meminfo_ops);
  register_file(proc, "tasks",   &tasks_ops);
  register_file(proc, "netinfo", &netinfo_ops);
  register_file(proc, "interrupts", &interrupts_ops);

  register_file(proc, "cmdline", &cmdline_ops);

  register_file(proc, "balloon", &balloon_ops);
  
  register_file(proc, "cpuinfo", &cpuinfo_ops);
  
  register_file(proc, "version", &version_ops);

  uart_println("[PROC] Mounted at /proc with uptime, meminfo, tasks, interrupts, netinfo, cmdline, version, balloon, cpuinfo");
}
