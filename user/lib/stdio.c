#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "sys.h"

/*
 * Buffered streams.
 *
 * Design notes
 * ------------
 * Separate read and write buffers are NOT used. A single buffer serves both,
 * with the invariant that at most one of the two buffered regions is non-empty:
 * switching direction flushes the other. That keeps the "what is in buf, and
 * what does it mean" question to a single flag check instead of two
 * interdependent counters.
 *
 * Every buffered byte position is tracked relative to `base`, the file offset
 * that buf[0] corresponds to. ftell() is then `base + (rpos or wpos)`, which
 * stays correct across flushes and partial fills without reconstructing state.
 *
 * Buffers are kmalloc'd per stream. A single shared static buffer would be the
 * same class of bug as the shared FAT32 sector buffer.
 */

#define STD_BUF_SIZE 512

/* Three preallocated streams over the inherited fds. Declared as plain objects
 * rather than pointers-to-objects like glibc, because nothing here benefits
 * from that indirection and it would let a user program reassign stdout. */
/* Buffers for the three inherited streams. They must be real storage, not
 * NULL: fputc() indexes buf[wpos] unconditionally once the stream is not
 * stderr, so a zero-initialised FILE would store to address 0. Giving the
 * preallocated streams statically allocated buffers (rather than kmalloc'ing
 * them in an init function) means stdout is usable from the first statement of
 * main, before any initialiser has run. */
static unsigned char in_buf[STD_BUF_SIZE];
static unsigned char out_buf[STD_BUF_SIZE];
static unsigned char err_buf[1]; /* stderr is written through unbuffered */

static FILE s_stdin  = { .fd = 0, .flags = _F_READ | _F_INUSE,
                         .buf = in_buf, .buf_size = STD_BUF_SIZE };
static FILE s_stdout = { .fd = 1, .flags = _F_WRITE | _F_INUSE,
                         .buf = out_buf, .buf_size = STD_BUF_SIZE };
static FILE s_stderr = { .fd = 2, .flags = _F_WRITE | _F_INUSE,
                         .buf = err_buf, .buf_size = 1 };

FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

/* Every open FILE, so fflush(NULL) and exit-time flushing can find them. Simple
 * linear scan; a program has very few streams open at once.
 *
 * The three inherited streams are pre-registered by static initialiser. They
 * must be: fflush(f) validates through this table, and a table that only ever
 * learned about fopen()ed files would make fflush(stdout) report failure for
 * the most common stream there is. */
#define MAX_OPEN_STREAMS 16
static FILE *streams[MAX_OPEN_STREAMS] = {
  &s_stdin, &s_stdout, &s_stderr,
};

static void stream_register(FILE *f) {
  for (int i = 0; i < MAX_OPEN_STREAMS; i++) {
    if (!streams[i]) {
      streams[i] = f;
      return;
    }
  }
}

static void stream_unregister(FILE *f) {
  for (int i = 0; i < MAX_OPEN_STREAMS; i++) {
    if (streams[i] == f) {
      streams[i] = 0;
      return;
    }
  }
}

static int stream_is_open(FILE *f) {
  for (int i = 0; i < MAX_OPEN_STREAMS; i++) {
    if (streams[i] == f) {
      return 1;
    }
  }
  return 0;
}

/* Push buffered output to the fd. Returns 0 or -1. */
static int stream_flush_write(FILE *f) {
  if (f->wpos == 0) {
    return 0;
  }
  long done = 0;
  while (done < (long)f->wpos) {
    long n = sys_write(f->fd, f->buf + done, f->wpos - (size_t)done);
    if (n <= 0) {
      f->flags |= _F_ERR;
      return -1;
    }
    done += n;
  }
  f->base += (long long)f->wpos;
  f->wpos = 0;
  return 0;
}

/* Discard buffered input.
 *
 * No lseek is needed, and performing one is a bug: after a buffered read the fd
 * is ALREADY positioned just past the buffered bytes (the read moved it), so
 * seeking forward again double-counts the whole buffer. An earlier version did
 * exactly that, which made every subsequent relative operation land hundreds of
 * bytes off.
 *
 * The only bookkeeping required is rebasing so that base + 0 is the file offset
 * the next operation will see. */
static int stream_discard_read(FILE *f) {
  f->base += (long long)f->rlen;
  f->rpos = f->rlen = 0;
  return 0;
}

/* Make the fd position correspond to buf[0]. */
static int stream_flush(FILE *f) {
  if (f->flags & _F_WRITE) {
    return stream_flush_write(f);
  }
  return stream_discard_read(f);
}

int fflush(FILE *f) {
  if (!f) {
    /* Flush every open stream. */
    int rc = 0;
    for (int i = 0; i < MAX_OPEN_STREAMS; i++) {
      if (streams[i] && stream_flush(streams[i]) < 0) {
        rc = -1;
      }
    }
    return rc;
  }
  if (!stream_is_open(f)) {
    return -1;
  }
  return stream_flush(f);
}

FILE *fopen(const char *path, const char *mode) {
  if (!path || !mode || !*mode) {
    errno = EINVAL;
    return NULL;
  }

  int flags = 0;
  switch (mode[0]) {
  case 'r':
    flags = O_RDONLY;
    if (mode[1] == '+') {
      flags = O_RDWR;
    }
    break;
  case 'w':
    flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (mode[1] == '+') {
      flags = O_RDWR | O_CREAT | O_TRUNC;
    }
    break;
  case 'a':
    flags = O_WRONLY | O_CREAT | O_APPEND;
    if (mode[1] == '+') {
      flags = O_RDWR | O_CREAT | O_APPEND;
    }
    break;
  default:
    errno = EINVAL;
    return NULL;
  }

  int fd = open(path, flags, 0644);
  if (fd < 0) {
    return NULL; /* errno already set by open() */
  }

  /* One allocation holds the FILE header and its buffer together, so the
   * stream can never exist without a buffer. */
  FILE *f = (FILE *)malloc(sizeof(FILE) + STD_BUF_SIZE);
  if (!f) {
    close(fd);
    errno = ENOMEM;
    return NULL;
  }
  memset(f, 0, sizeof(FILE));
  f->fd      = fd;
  f->buf     = (unsigned char *)(f + 1); /* buffer lives in the same block */
  f->buf_size = STD_BUF_SIZE;
  f->base    = 0;
  f->flags   = _F_INUSE | ((flags & O_ACCMODE) == O_RDONLY ? _F_READ : _F_WRITE);
  if (flags == O_RDWR) {
    f->flags |= _F_READ;
  }

  if (flags & O_APPEND) {
    long end = sys_lseek(fd, 0, SEEK_END);
    if (end >= 0) {
      f->base = end;
    }
  }

  stream_register(f);
  return f;
}

int fclose(FILE *f) {
  if (!f || !stream_is_open(f)) {
    return EOF;
  }
  int rc = stream_flush(f);
  close(f->fd);
  stream_unregister(f);
  free(f);
  return rc;
}

/* ---- raw-ish I/O through the buffer ------------------------------------ */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
  if (!f || !size || !nmemb || !(f->flags & _F_READ)) {
    /* Distinguish "not a readable stream" from a genuine short read: a caller
     * that gets 0 items back needs to know whether to retry or to report
     * EBADF. The kernel sets errno for syscalls; libc has to do the same for
     * its own layer. */
    if (f && !(f->flags & _F_READ)) {
      errno = EBADF;
    }
    return 0;
  }
  size_t want = size * nmemb;
  unsigned char *out = (unsigned char *)ptr;
  size_t done = 0;

  /* Hand back whatever is already buffered before touching the fd. */
  while (done < want && f->rpos < f->rlen) {
    size_t avail = f->rlen - f->rpos;
    size_t take  = want - done;
    if (take > avail) {
      take = avail;
    }
    memcpy(out + done, f->buf + f->rpos, take);
    f->rpos += take;
    done += take;
  }
  if (done == want) {
    return nmemb;
  }

  /* For a request larger than the buffer, go straight to the fd: staging
   * through the buffer would just add a copy. */
  if (want - done >= f->buf_size) {
    while (done < want) {
      long n = sys_read(f->fd, out + done, want - done);
      if (n <= 0) {
        if (n == 0) {
          f->flags |= _F_EOF;
        } else {
          f->flags |= _F_ERR;
        }
        break;
      }
      done += (size_t)n;
    }
    f->base += (long long)done;
    return done / size;
  }

  /* Otherwise top up the buffer and copy out of it.
   *
   * Record the file offset the buffer was filled from, because that offset is
   * now only known to the kernel (the fd has moved past it) and ftell() plus
   * fseek(SEEK_CUR) both need it. */
  long pos = sys_lseek(f->fd, 0, SEEK_CUR);
  if (pos < 0) {
    f->flags |= _F_ERR;
    return done / size;
  }
  long n = sys_read(f->fd, f->buf, f->buf_size);
  if (n <= 0) {
    f->flags |= (n == 0) ? _F_EOF : _F_ERR;
    return done / size;
  }
  f->base = pos;
  f->rpos = 0;
  f->rlen = (size_t)n;
  while (done < want && f->rpos < f->rlen) {
    size_t avail = f->rlen - f->rpos;
    size_t take  = want - done;
    if (take > avail) {
      take = avail;
    }
    memcpy(out + done, f->buf + f->rpos, take);
    f->rpos += take;
    done += take;
  }
  return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
  if (!f || !size || !nmemb || !(f->flags & _F_WRITE)) {
    if (f && !(f->flags & _F_WRITE)) {
      errno = EBADF;
    }
    return 0;
  }
  size_t want  = size * nmemb;
  size_t total = 0;
  const unsigned char *in = (const unsigned char *)ptr;

  while (total < want) {
    size_t space = f->buf_size - f->wpos;
    if (space == 0) {
      if (stream_flush_write(f) < 0) {
        break;
      }
      space = f->buf_size;
    }

    /* Large writes bypass the buffer entirely. */
    if (want - total >= f->buf_size) {
      long n = sys_write(f->fd, in + total, want - total);
      if (n <= 0) {
        f->flags |= _F_ERR;
        break;
      }
      total += (size_t)n;
      f->base += n;
      continue;
    }

    size_t take = space;
    if (take > want - total) {
      take = want - total;
    }
    memcpy(f->buf + f->wpos, in + total, take);
    f->wpos += take;
    total += take;
  }
  return total / size;
}

/* ---- positioning -------------------------------------------------------- */

int fseek(FILE *f, long offset, int whence) {
  if (!f || !stream_is_open(f)) {
    return -1;
  }
  /* Capture the logical position BEFORE flushing.
   *
   * stream_flush() advances the fd past whatever is buffered and rebases
   * `base` to buf[0]'s offset, discarding rpos/wpos. Computing a SEEK_CUR
   * target afterwards loses however far into the buffer we had read, so
   * "seek back 8 from here" landed 511 bytes too far. */
  long logical = (long)f->base +
                 ((f->flags & _F_WRITE) ? (long)f->wpos : (long)f->rpos);

  /* Flush whichever region is buffered so the fd position is meaningful. */
  if (stream_flush(f) < 0) {
    return -1;
  }

  long target;
  if (whence == SEEK_SET) {
    target = offset;
  } else if (whence == SEEK_CUR) {
    target = logical + offset;
  } else if (whence == SEEK_END) {
    long end = sys_lseek(f->fd, 0, SEEK_END);
    if (end < 0) {
      return -1;
    }
    target = end + offset;
  } else {
    errno = EINVAL;
    return -1;
  }

  if (target < 0) {
    errno = EINVAL;
    return -1;
  }
  if (sys_lseek(f->fd, target, SEEK_SET) < 0) {
    return -1;
  }
  f->base = target;
  f->rpos = f->rlen = f->wpos = 0;
  f->flags &= ~_F_EOF;
  return 0;
}

long ftell(FILE *f) {
  if (!f || !stream_is_open(f)) {
    return -1;
  }
  /* base is the fd offset for buf[0]; add how far into the buffer we are. */
  long within = (f->flags & _F_WRITE) ? (long)f->wpos : (long)f->rpos;
  return (long)f->base + within;
}

void clearerr(FILE *f) {
  if (f) {
    f->flags &= ~(_F_EOF | _F_ERR);
  }
}

void rewind(FILE *f) {
  fseek(f, 0, SEEK_SET);
  clearerr(f);
}

int feof(FILE *f)   { return f && (f->flags & _F_EOF) ? 1 : 0; }
int ferror(FILE *f) { return f && (f->flags & _F_ERR) ? 1 : 0; }

/* ---- character I/O ------------------------------------------------------ */

int fgetc(FILE *f) {
  if (!f || !(f->flags & _F_READ)) {
    if (f && !(f->flags & _F_READ)) {
      errno = EBADF;
    }
    return EOF;
  }
  if (f->rpos < f->rlen) {
    return (int)f->buf[f->rpos++];
  }
  long pos = sys_lseek(f->fd, 0, SEEK_CUR);
  if (pos < 0) {
    f->flags |= _F_ERR;
    return EOF;
  }
  long n = sys_read(f->fd, f->buf, f->buf_size);
  if (n <= 0) {
    f->flags |= (n == 0) ? _F_EOF : _F_ERR;
    return EOF;
  }
  f->base = pos;
  f->rpos = 1;
  f->rlen = (size_t)n;
  return (int)f->buf[0];
}

int getc(FILE *f)    { return fgetc(f); }
int getchar(void)    { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
  /* Only the immediately-preceding byte can be pushed back, and never at EOF.
   * A one-byte pushback is all this implementation supports; returning EOF for
   * anything else is honest rather than silently corrupting the stream. */
  if (!f || !(f->flags & _F_READ) || c == EOF || f->rpos == 0) {
    return EOF;
  }
  if (f->rpos <= f->rlen) {
    f->buf[--f->rpos] = (unsigned char)c;
    f->flags &= ~_F_EOF;
    return c;
  }
  return EOF;
}

char *fgets(char *s, int size, FILE *f) {
  if (!s || size <= 0 || !f) {
    return NULL;
  }
  int i = 0;
  while (i < size - 1) {
    int c = fgetc(f);
    if (c == EOF) {
      break;
    }
    s[i++] = (char)c;
    if (c == '\n') {
      break; /* the newline itself IS part of the result, per the standard */
    }
  }
  if (i == 0) {
    return NULL; /* immediate EOF yields no line at all */
  }
  s[i] = '\0';
  return s;
}

int fputc(int c, FILE *f) {
  if (!f || !(f->flags & _F_WRITE)) {
    if (f && !(f->flags & _F_WRITE)) {
      errno = EBADF;
    }
    return EOF;
  }
  unsigned char ch = (unsigned char)c;
  /* stderr is unbuffered: it is the channel for diagnostics, and buffering it
   * would reorder them relative to buffered stdout. */
  if (f == stderr) {
    sys_write(f->fd, &ch, 1);
    return (int)ch;
  }
  if (f->wpos == f->buf_size) {
    if (stream_flush_write(f) < 0) {
      return EOF;
    }
  }
  f->buf[f->wpos++] = ch;
  return (int)ch;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int putchar(int c) {
  return fputc(c, stdout);
}

int fputs(const char *s, FILE *f) {
  if (!s || !f) {
    return EOF;
  }
  size_t n = strlen(s);
  return (fwrite(s, 1, n, f) == n) ? 0 : EOF;
}

int puts(const char *s) {
  if (!s) {
    return EOF;
  }
  if (fputs(s, stdout) == EOF) {
    return EOF;
  }
  return (fputc('\n', stdout) == EOF) ? EOF : 0;
}

int remove(const char *path) { (void)path; errno = EOPNOTSUPP; return -1; }
int rename(const char *from, const char *to) {
  (void)from;
  (void)to;
  errno = EOPNOTSUPP;
  return -1;
}

void _exit(int status) { sys_exit(); (void)status; }
