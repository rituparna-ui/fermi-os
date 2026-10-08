/*
 * <dirent.h> implementation — POSIX directory enumeration over getdents64(2).
 *
 * The kernel's getdents64 fills a caller buffer with a run of variable-length
 * records and reports how many bytes it wrote. That is a streaming interface,
 * not a one-entry-at-a-time one, so readdir() has to do two things: ask the
 * kernel for a batch, then hand back one record at a time from that batch until
 * it is exhausted, refilling as needed.
 *
 * The resume cookie is the descriptor's own file offset, kept by the kernel per
 * descriptor. That is what makes two independent DIR streams over the same
 * directory behave separately, and it is why rewinddir() can be implemented by
 * simply seeking the descriptor back to zero.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* getdents64(2), Linux aarch64 number 61. */
#define SYS_GETDENTS64 61

/* Offsets inside the kernel's struct linux_dirent64. */
#define D_INO_OFF    0
#define D_OFF_OFF    8
#define D_RECLEN_OFF 16
#define D_TYPE_OFF   18
#define D_NAME_OFF   19

static long sys_getdents64(int fd, void *buf, unsigned long count) {
  register long     x0 __asm__("x0") = fd;
  register long     x1 __asm__("x1") = (long)buf;
  register long     x2 __asm__("x2") = (long)count;
  register uint64_t x8 __asm__("x8") = SYS_GETDENTS64;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return (long)x0;
}

DIR *opendir(const char *path) {
  if (!path) {
    errno = EFAULT;
    return NULL;
  }

  /* O_RDONLY only. A directory is never written, and O_DIRECTORY is not
   * implemented, so a caller asking for it would get a directory fd it has no
   * reason to want. */
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return NULL; /* open() already set errno */
  }

  DIR *d = calloc(1, sizeof(DIR));
  if (!d) {
    /* Do not leak the descriptor on the failure path: a caller that retries
     * would exhaust the fd table without ever knowing why. */
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  /* Type-check by making the first real read.
   *
   * open(2) deliberately succeeds on a regular file -- refusing there would
   * break cat(1) and everything else that opens files -- so opendir has to
   * establish "this is a directory" itself. The kernel answers that with ENOTDIR
   * from getdents64, and performing the check as the first read rather than a
   * separate probe costs no extra syscall: the batch it produces is the batch
   * readdir was going to fetch anyway. */
  long got = sys_getdents64(fd, d->buf, sizeof(d->buf));
  if (got < 0) {
    /* Save errno before free/close, which are free to clobber it. */
    int saved = errno;
    free(d);
    close(fd);
    errno = saved;
    return NULL;
  }

  d->fd       = fd;
  d->next_off = 0;
  d->eof      = 0;
  d->buf_len  = (size_t)got;
  d->buf_pos  = 0;
  return d;
}

struct dirent *readdir(DIR *d) {
  if (!d) {
    errno = EFAULT;
    return NULL;
  }

  /* Exhausted the last batch already. */
  if (d->eof) {
    return NULL;
  }

  for (;;) {
    /* Serve from the batch we already have, if any is left. */
    if (d->buf_pos < d->buf_len) {
      unsigned char *rec = d->buf + d->buf_pos;
      unsigned short reclen = (unsigned short)rec[D_RECLEN_OFF] |
                              ((unsigned short)rec[D_RECLEN_OFF + 1] << 8);

      /* A zero or nonsensical reclen would make the cursor stand still and spin
       * forever. Treat it as a corrupt record and stop rather than loop. */
      if (reclen == 0 || d->buf_pos + reclen > d->buf_len) {
        d->eof = 1;
        errno = EIO;
        return NULL;
      }

      d->buf_pos += reclen;

      d->cur.d_ino    = *(ino_t *)(rec + D_INO_OFF);
      d->cur.d_off    = *(long *)(rec + D_OFF_OFF);
      d->cur.d_reclen = reclen;
      d->cur.d_type   = rec[D_TYPE_OFF];

      /* The kernel does NOT NUL-terminate the name in the record; the length is
       * implied by reclen. Copy it out with an explicit bound rather than
       * strcpy, since the name is known not to be terminated. */
      const char *name  = (const char *)(rec + D_NAME_OFF);
      size_t      avail = (size_t)reclen - D_NAME_OFF;
      size_t      n     = 0;
      while (n < avail && name[n] != '\0' && n < NAME_MAX) {
        d->cur.d_name[n] = name[n];
        n++;
      }
      d->cur.d_name[n] = '\0';

      return &d->cur;
    }

    /* Batch exhausted: ask the kernel for more. */
    long got = sys_getdents64(d->fd, d->buf, sizeof(d->buf));
    if (got < 0) {
      /* The kernel sets errno via TPIDR_EL0 before returning -1. */
      return NULL;
    }
    if (got == 0) {
      d->eof = 1;
      return NULL; /* end of directory: errno left untouched, per POSIX */
    }
    d->buf_len = (size_t)got;
    d->buf_pos = 0;
  }
}

long telldir(DIR *d) {
  if (!d) {
    errno = EFAULT;
    return -1;
  }
  /* The cookie the kernel will resume from: everything in the current batch has
   * been consumed up to buf_pos, and d_off of the last returned record is the
   * position after it. */
  return d->cur.d_off;
}

void rewinddir(DIR *d) {
  if (!d) {
    return;
  }
  /* Rewinding means "start over from the beginning". The kernel's cookie lives
   * in the descriptor's offset, so seeking it to zero is the whole operation;
   * the cached batch is dropped because it describes the old position. */
  lseek(d->fd, 0, SEEK_SET);
  d->buf_pos = 0;
  d->buf_len = 0;
  d->eof     = 0;
}

int closedir(DIR *d) {
  if (!d) {
    errno = EFAULT;
    return -1;
  }
  int fd = d->fd;
  free(d);
  return close(fd);
}