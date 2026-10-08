#ifndef USER_DIRENT_H
#define USER_DIRENT_H

#include <stddef.h>
#include <sys.h>

/*
 * <dirent.h> — directory enumeration, POSIX style.
 *
 * Fermi libc (user/lib/dirent.c).
 *
 * Built on the kernel's getdents64(2), which is the same raw call a Linux binary
 * makes. The two differ only in presentation: the kernel hands back
 * `struct linux_dirent64` with an unterminated name, and readdir() turns each
 * record into the NUL-terminated `struct dirent` POSIX specifies.
 */

/* Maximum length of a single path component. */
#define NAME_MAX 255

typedef unsigned long ino_t;

/*
 * One directory entry.
 *
 * The kernel's wire format leaves d_name unterminated and sized by d_reclen;
 * here it is a real array. The trade is a copy per entry, which is irrelevant at
 * this scale and buys the guarantee that d_name is always readable.
 */
struct dirent {
  ino_t          d_ino;
  long           d_off;   /* opaque; usable as a seek cookie (telldir) */
  unsigned short d_reclen;
  unsigned char  d_type;
  char           d_name[NAME_MAX + 1];
};

/* d_type values. Deliberately the Linux numbers, so the same constants describe
 * what the kernel reports and what readdir() passes through. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8

/* Opaque directory stream. */
typedef struct {
  int            fd;
  long           next_off;  /* getdents64 cookie to resume from */
  int            eof;
  unsigned char  buf[1024];
  size_t         buf_len;   /* bytes of valid records in buf */
  size_t         buf_pos;   /* offset of the next unconsumed record */
  /* The record readdir() hands back lives here rather than pointing into buf, so
   * the returned pointer stays valid for exactly as long as POSIX promises. */
  struct dirent  cur;
} DIR;

DIR       *opendir(const char *path);
struct dirent *readdir(DIR *d);
int        closedir(DIR *d);
long       telldir(DIR *d);
void       rewinddir(DIR *d);

#endif