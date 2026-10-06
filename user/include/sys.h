/*
 * user/include/sys.h — minimal libc-style syscall wrappers for Fermi OS
 *  user programs. Header-only so user .c sources can just #include this and
 * compile/link with crt0.o, no separate libc.a needed.
 *
 * The numbering here MUST stay in sync with src/syscall/syscall.h. We
 * intentionally don't include the kernel header from user code — it
 * pulls in kernel-side types and headers that wouldn't compile here.
 *
 * AAPCS64 syscall convention:
 *   x8 = syscall number
 *   x0..x7 = args, return value in x0
 */
#ifndef USER_SYS_H
#define USER_SYS_H

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;
typedef unsigned long      uintptr_t; /* needed by __fermi_errno_loc() below */
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_EXIT    4
#define SYS_YIELD   5
#define SYS_SLEEP   6
#define SYS_GETPID  7
#define SYS_LSEEK   8
#define SYS_UPTIME  9
#define SYS_NET_PING 10
#define SYS_KILL    11
#define SYS_FORK    12
#define SYS_EXEC    13
#define SYS_BALLOON 14

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/*
 * errno
 *
 * The kernel keeps a per-task TLS page and points TPIDR_EL0 at it; errno is
 * the int at offset 0. Reading TPIDR_EL0 is the only correct way to reach it —
 * a plain `int errno;` would be one global shared by every task, so two
 * processes would clobber each other's error codes.
 *
 * This is the same shape Linux uses (thread pointer in TPIDR_EL0, errno at the
 * TCB base), so a libc can implement __errno_location() the same way.
 *
 * Declared as a function rather than a macro so every access compiles to one
 * MRS. The `static inline` is fine: the returned pointer is to live kernel
 * memory, and TLS addresses stay valid for the life of the process.
 */
static inline int *__fermi_errno_loc(void) {
  uintptr_t tpidr;
  __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tpidr));
  return (int *)tpidr; /* TLS_ERRNO_OFFSET == 0 */
}

/* POSIX requires `errno` to be a modifiable lvalue of type int. A macro
 * expanding to (*__fermi_errno_loc()) satisfies that without giving user code
 * a writable global. */
#define errno (*__fermi_errno_loc())

/* errno values — standard Linux/POSIX numbers, matching syscall.h. */
#define EPERM      1
#define ENOENT     2
#define ESRCH      3
#define EINTR      4
#define EIO        5
#define ENXIO      6
#define E2BIG      7
#define EBADF      9
#define EAGAIN     11
#define ENOMEM     12
#define EACCES     13
#define EFAULT     14
#define EEXIST     17
#define ENODEV     19
#define ENOTDIR    20
#define EISDIR     21
#define EINVAL     22
#define ENFILE     23
#define EMFILE     24
#define ENOSPC     28
#define ESPIPE     29
#define EROFS      30
#define ERANGE     34
#define ENOSYS     38
#define ENOTEMPTY  39
#define EOPNOTSUPP 95

/* MAP_ and PROT_ flags, plus the mmap-family syscall numbers. */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define SYS_BRK      214
#define SYS_MUNMAP   215
#define SYS_MMAP     222
#define SYS_MPROTECT 226

/* MAP_FAILED — what mmap returns on error. */
#define MAP_FAILED ((void *)-1)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

static inline ssize_t sys_read(int fd, void *buf, size_t count) {
  register long      x0 __asm__("x0") = fd;
  register void     *x1 __asm__("x1") = buf;
  register size_t    x2 __asm__("x2") = count;
  register uint64_t  x8 __asm__("x8") = SYS_READ;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (ssize_t)x0;
}

static inline ssize_t sys_write(int fd, const void *buf, size_t count) {
  register long       x0 __asm__("x0") = fd;
  register const void *x1 __asm__("x1") = buf;
  register size_t     x2 __asm__("x2") = count;
  register uint64_t   x8 __asm__("x8") = SYS_WRITE;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (ssize_t)x0;
}

static inline int sys_open(const char *path) {
  register const char *x0 __asm__("x0") = path;
  register uint64_t    x8 __asm__("x8") = SYS_OPEN;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (int)(long)x0;
}

static inline int sys_close(int fd) {
  register long     x0 __asm__("x0") = fd;
  register uint64_t x8 __asm__("x8") = SYS_CLOSE;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (int)x0;
}

static inline void sys_exit(void) {
  register uint64_t x8 __asm__("x8") = SYS_EXIT;
  __asm__ __volatile__("svc #0" ::"r"(x8) : "memory");
  __builtin_unreachable();
}

static inline int sys_getpid(void) {
  register long     x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = SYS_GETPID;
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return (int)x0;
}

static inline int64_t sys_lseek(int fd, int64_t off, int whence) {
  register long     x0 __asm__("x0") = fd;
  register int64_t  x1 __asm__("x1") = off;
  register long     x2 __asm__("x2") = whence;
  register uint64_t x8 __asm__("x8") = SYS_LSEEK;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (int64_t)x0;
}

static inline uint64_t sys_uptime(void) {
  register uint64_t x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = SYS_UPTIME;
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return x0;
}

/*
 * mmap family wrappers.
 *
 * These take the Linux prototypes so user code can be written against
 * <sys/mman.h> semantics and later swapped for the real header without
 * touching call sites.
 */
static inline void *sys_mmap(void *addr, unsigned long length, int prot,
                             int flags, int fd, long offset) {
  register long x0 __asm__("x0") = (long)addr;
  register long x1 __asm__("x1") = (long)length;
  register long x2 __asm__("x2") = (long)prot;
  register long x3 __asm__("x3") = (long)flags;
  register long x4 __asm__("x4") = (long)fd;
  register long x5 __asm__("x5") = (long)offset;
  register uint64_t x8 __asm__("x8") = SYS_MMAP;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                       : "memory");
  return (void *)x0;
}

static inline int sys_munmap(void *addr, unsigned long length) {
  register long x0 __asm__("x0") = (long)addr;
  register long x1 __asm__("x1") = (long)length;
  register uint64_t x8 __asm__("x8") = SYS_MUNMAP;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return (int)x0;
}

static inline int sys_mprotect(void *addr, unsigned long length, int prot) {
  register long x0 __asm__("x0") = (long)addr;
  register long x1 __asm__("x1") = (long)length;
  register long x2 __asm__("x2") = (long)prot;
  register uint64_t x8 __asm__("x8") = SYS_MPROTECT;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return (int)x0;
}

static inline void *sys_brk(void *addr) {
  register long x0 __asm__("x0") = (long)addr;
  register uint64_t x8 __asm__("x8") = SYS_BRK;
  __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return (void *)x0;
}

/* SYS_FORK returns the child pid in x0, and 0 in the child. It must be read
 * from x0 — x8 still holds the syscall number afterwards. */
static inline int sys_fork(void) {
  register long     x0 __asm__("x0");
  register uint64_t x8 __asm__("x8") = SYS_FORK;
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
  return (int)x0;
}

/* Minimal unsigned decimal renderer, enough for test reporting until the real
 * libc's strtoul/printf land. Returns the number of bytes written. */
static inline int u_utoa(char *buf, int max, unsigned long v) {
  if (max < 1) return 0;
  if (v == 0) { buf[0] = '0'; return 1; }
  char tmp[24];
  int n = 0;
  while (v > 0 && n < (int)sizeof(tmp)) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  int out = 0;
  while (n > 0 && out < max) buf[out++] = tmp[--n];
  return out;
}

static inline void sys_sleep(uint64_t ms) {
  register uint64_t x0 __asm__("x0") = ms;
  register uint64_t x8 __asm__("x8") = SYS_SLEEP;
  __asm__ __volatile__("svc #0" ::"r"(x0), "r"(x8) : "memory");
}

/* Convenience helpers: not syscalls themselves, just small inlines that
 * almost every user program needs and we don't want to redefine in each. */
static inline size_t u_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

static inline void u_puts(const char *s) {
  sys_write(STDOUT_FILENO, s, u_strlen(s));
}

#endif /* USER_SYS_H */
