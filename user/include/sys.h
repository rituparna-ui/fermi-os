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
