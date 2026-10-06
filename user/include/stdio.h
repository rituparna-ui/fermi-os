#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include "sys.h"

/*
 * <stdio.h> — buffered streams.
 *
 * Part of Fermi libc (user/lib/stdio.c, user/lib/printf.c).
 */

/* EOF is -1 by definition. Defined here rather than relying on the host's
 * <stdio.h>, which is not available in a freestanding build. */
#define EOF (-1)

typedef struct _FILE {
  int      fd;
  int      flags;    /* internal state bits, below */
  size_t   buf_size; /* buffer capacity in bytes */
  unsigned char *buf;
  /* Buffered-read state. `rpos` is the next byte to hand out, `rlen` how many
   * are valid. Keeping them separate (rather than a single pointer) means a
   * short read mid-buffer is visible instead of silently losing data. */
  size_t   rpos;
  size_t   rlen;
  /* Buffered-write state: bytes sitting in buf[0, wpos) not yet written. */
  size_t   wpos;
  /* Byte offset of buf[0] in the underlying file. Everything above is relative
   * to this, which is what makes ftell() correct across a partial flush. */
  long long base;
} FILE;

#define _IOFBF 0   /* buffered (the only mode implemented) */
#define _IOLBF 1
#define _IONBF 2

/* flags bits */
#define _F_READ  0x0001
#define _F_WRITE 0x0002
#define _F_EOF   0x0004
#define _F_ERR   0x0008
#define _F_INUSE 0x0010

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);

int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    fflush(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *s, int size, FILE *f);

int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);

int    remove(const char *path);
int    rename(const char *from, const char *to);

/* Formatted output. Float conversions (%f/%e/%g) are not implemented yet and
 * print as a diagnostic rather than silently emitting nothing. */
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

void perror(const char *prefix);

#endif