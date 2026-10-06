#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/*
 * Formatted output engine.
 *
 * Design: one sink-agnostic core (vformat) that renders into a bounded byte
 * buffer, plus thin wrappers. Every wrapper writes through the same code, so
 * sprintf and printf cannot drift apart — which is the usual fate of "one
 * implementation for snprintf, a second for printf".
 *
 * Sink contract:
 *   put(dst, n, ctx) returns the number of bytes accepted, and may accept
 *   fewer than n (that's how truncation works). vformat always keeps counting
 *   the bytes it *would* have written, so the return value matches what
 *   snprintf must report: the length the full result needed, excluding the NUL.
 *
 * Supported: %d %i %u %o %x %X %c %s %p %% with field width, precision, the
 * '-', '+', ' ', '0' and '#' flags, '*' width/precision, and the hh/h/l/ll/z/j/t
 * length modifiers.
 *
 * NOT supported: floating point (%f %e %g %a). Those conversions need
 * correctly-rounded decimal expansion of a binary fraction, which means
 * 128-bit integer arithmetic or a soft-float helper. Rather than print
 * garbage, an unimplemented conversion emits a visible marker so a caller
 * finds out immediately. This is the main remaining gap in the libc.
 */

/* ---- sink ---------------------------------------------------------------- */

typedef struct {
  char   *buf;    /* NULL for a stream sink */
  FILE   *stream; /* NULL for a buffer sink */
  size_t  size;   /* buffer capacity including the NUL slot */
  size_t  used;   /* bytes actually stored (never exceeds size-1) */
  size_t  total;  /* bytes the full result needs, excluding the NUL */
} sink_t;

static void sink_put(sink_t *s, const char *data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = data[i];
    s->total++;
    if (s->buf) {
      if (s->used + 1 < s->size) {
        s->buf[s->used] = c;
        s->used++;
      }
      /* else: truncated; the byte is counted but not stored */
    } else {
      putc(c, s->stream);
    }
  }
}

static void sink_pad(sink_t *s, char c, int n) {
  for (int i = 0; i < n; i++) {
    sink_put(s, &c, 1);
  }
}

/* ---- number conversion -------------------------------------------------- */

static const char kHexLower[] = "0123456789abcdef";
static const char kHexUpper[] = "0123456789ABCDEF";

/* Render `value` in `base` into buf (which must hold at least 64 bytes) and
 * return its length. `upper` selects the digit alphabet. */
static int utoa_buf(char *buf, unsigned long long value, unsigned base,
                    int upper) {
  const char *digits = upper ? kHexUpper : kHexLower;
  char        tmp[64];
  int         n = 0;

  if (value == 0) {
    tmp[n++] = '0';
  }
  while (value > 0 && n < (int)sizeof(tmp)) {
    tmp[n++] = digits[value % base];
    value /= base;
  }
  /* tmp is least-significant-first; emit reversed. */
  for (int i = 0; i < n; i++) {
    buf[i] = tmp[n - 1 - i];
  }
  return n;
}

/* ---- the formatter ------------------------------------------------------ */

#define FLAG_LEFT  0x01
#define FLAG_PLUS  0x02
#define FLAG_SPACE 0x04
#define FLAG_ZERO  0x08
#define FLAG_ALT   0x10 /* '#' */

/* Core. `out_stream` NULL means render into sink->buf. */
int vformat(sink_t *s, const char *fmt, va_list ap) {
  while (*fmt) {
    /* Emit the literal run up to the next '%'. */
    const char *start = fmt;
    while (*fmt && *fmt != '%') {
      fmt++;
    }
    if (fmt != start) {
      sink_put(s, start, (size_t)(fmt - start));
    }
    if (!*fmt) {
      break;
    }
    /* Consume the '%' here, unconditionally and exactly once.
     *
     * An earlier version consumed it only inside the "was this a literal run"
     * branch, so a format string STARTING with a conversion ("%d") fell
     * through to flag parsing while still pointing at '%'. It then matched
     * conv == '%' and emitted a literal percent — so "%d" printed "%d". */
    fmt++;

    int flags = 0;

    /* flag characters, in any order */
    for (;;) {
      if (*fmt == '-') { flags |= FLAG_LEFT;  fmt++; }
      else if (*fmt == '+') { flags |= FLAG_PLUS; fmt++; }
      else if (*fmt == ' ') { flags |= FLAG_SPACE; fmt++; }
      else if (*fmt == '0') { flags |= FLAG_ZERO; fmt++; }
      else if (*fmt == '#') { flags |= FLAG_ALT; fmt++; }
      else break;
    }

    /* width */
    int width = 0;
    if (*fmt == '*') {
      /* A negative width from '*' means LEFT, per the standard. */
      width = va_arg(ap, int);
      fmt++;
      if (width < 0) {
        flags |= FLAG_LEFT;
        width = -width;
      }
    } else {
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }
    }

    /* precision (-1 = unspecified) */
    int prec = -1;
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
        if (prec < 0) {
          prec = -1; /* negative precision via '*' means "unspecified" */
        }
      } else {
        while (*fmt >= '0' && *fmt <= '9') {
          prec = prec * 10 + (*fmt - '0');
          fmt++;
        }
      }
    }

    /* length modifier — collected, then applied to the integer fetch */
    int lmod = 0; /* 'h'=1, 'hh'=2, 'l'=3, 'll'=4, 'z'=5, 'j'=6, 't'=7 */
    if (*fmt == 'h') {
      fmt++;
      lmod = (*fmt == 'h') ? (fmt++, 2) : 1;
    } else if (*fmt == 'l') {
      fmt++;
      lmod = (*fmt == 'l') ? (fmt++, 4) : 3;
    } else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
      lmod = (*fmt == 'z') ? 5 : (*fmt == 'j') ? 6 : 7;
      fmt++;
    }

    char conv = *fmt;
    if (!conv) {
      break; /* trailing '%' with no conversion: stop rather than spin */
    }
    fmt++;

    /* ---- literal and pass-through conversions ---- */
    if (conv == '%') {
      sink_put(s, "%", 1);
      continue;
    }
    if (conv == 'c') {
      char c = (char)va_arg(ap, int);
      if (!(flags & FLAG_LEFT)) {
        sink_pad(s, ' ', width - 1);
      }
      sink_put(s, &c, 1);
      if (flags & FLAG_LEFT) {
        sink_pad(s, ' ', width - 1);
      }
      continue;
    }
    if (conv == 's') {
      const char *str = va_arg(ap, const char *);
      if (!str) {
        str = "(null)"; /* glibc convention; printing NULL would fault */
      }
      /* A precision truncates the string; it is a maximum output length, not a
       * count of source bytes. */
      int len = prec >= 0 ? (int)strnlen(str, (size_t)prec) : (int)strlen(str);
      if (!(flags & FLAG_LEFT)) {
        sink_pad(s, ' ', width - len);
      }
      sink_put(s, str, (size_t)len);
      if (flags & FLAG_LEFT) {
        sink_pad(s, ' ', width - len);
      }
      continue;
    }
    if (conv == 'p') {
      void *ptr = va_arg(ap, void *);
      /* Always emit the full width so pointers line up in a column. */
      if (!(flags & FLAG_LEFT)) {
        sink_pad(s, ' ', width - 18);
      }
      char tmp[32];
      int  n = utoa_buf(tmp, (unsigned long long)(uintptr_t)ptr, 16, 0);
      sink_put(s, "0x", 2);
      sink_put(s, tmp, (size_t)n);
      if (flags & FLAG_LEFT) {
        sink_pad(s, ' ', width - 18);
      }
      continue;
    }

    /* ---- integer conversions ---- */
    int is_signed = (conv == 'd' || conv == 'i');

    unsigned long long uv;
    /* `negative` is recorded separately from the magnitude. An earlier version
     * recomputed the sign by casting the unsigned magnitude back to signed,
     * which is ALWAYS non-negative — so every negative number printed without
     * its sign. */
    int negative = 0;

    if (is_signed) {
      long long sv;
      switch (lmod) {
      case 2: sv = (signed char)va_arg(ap, int);  break;
      case 1: sv = (short)va_arg(ap, int);      break;
      case 3: sv = va_arg(ap, long);            break;
      case 4: sv = va_arg(ap, long long);       break;
      case 5: sv = (long)va_arg(ap, long);      break; /* ssize_t */
      case 6: sv = (long)va_arg(ap, long);      break; /* intmax_t */
      case 7: sv = (long)va_arg(ap, long);      break; /* ptrdiff_t */
      default: sv = va_arg(ap, int);            break;
      }
      negative = (sv < 0);
      /* Take the magnitude in unsigned space: negating LLONG_MIN as a signed
       * value overflows, and that is a value real programs print. */
      uv = negative ? (unsigned long long)(-(sv + 1)) + 1ULL
                    : (unsigned long long)sv;
    } else {
      switch (lmod) {
      case 2: uv = (unsigned char)va_arg(ap, unsigned int);  break;
      case 1: uv = (unsigned short)va_arg(ap, unsigned int); break;
      case 3: uv = va_arg(ap, unsigned long);                break;
      case 4: uv = va_arg(ap, unsigned long long);           break;
      case 5: uv = va_arg(ap, unsigned long);                break; /* size_t */
      case 6: uv = va_arg(ap, unsigned long);                break;
      case 7: uv = va_arg(ap, unsigned long);                break;
      default: uv = va_arg(ap, unsigned int);                break;
      }
    }

    unsigned base = 10;
    int upper = 0;
    switch (conv) {
    case 'u': base = 10; break;
    case 'o': base = 8;  break;
    case 'x': base = 16; break;
    case 'X': base = 16; upper = 1; break;
    case 'd': case 'i': base = 10; break;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
    case 'a': case 'A':
      /* Not implemented. Emit a marker so the bug is visible at the call site
       * instead of printing an empty field that looks like an empty value. */
      sink_put(s, "<float unsupported>", 19);
      continue;
    default:
      /* Unknown conversion: echo it verbatim so the mistake is visible. */
      sink_put(s, "%", 1);
      sink_put(s, &conv, 1);
      continue;
    }

    char digits[72];
    int  nlen = utoa_buf(digits, uv, base, upper);

    /* Sign, from the recorded flag rather than the magnitude. */
    const char *sign = "";
    if (is_signed) {
      if (negative) {
        sign = "-";
      } else if (flags & FLAG_PLUS) {
        sign = "+";
      } else if (flags & FLAG_SPACE) {
        sign = " ";
      }
    }

    /* Alternate form: 0x/0X for hex, leading 0 for octal. */
    char alt[2] = {0, 0};
    int  altlen = 0;
    if ((flags & FLAG_ALT) && uv != 0) {
      if (base == 16) {
        alt[0] = '0';
        alt[1] = upper ? 'X' : 'x';
        altlen = 2;
      } else if (base == 8 && digits[0] != '0') {
        alt[0] = '0';
        altlen = 1;
      }
    }

    /* Precision on an integer means "at least this many digits", zero-filled
     * on the left, and it SUPPRESSES the '0' flag (standard behaviour). */
    int zero_digits = (prec > nlen) ? (prec - nlen) : 0;
    int used  = (int)strlen(sign) + altlen + zero_digits + nlen;
    int total_pad = width - used;

    /* '0' padding only applies when no precision was given, and never when
     * left-aligning. */
    int pad_with_zero = (flags & FLAG_ZERO) && prec < 0 && !(flags & FLAG_LEFT);

    /* Leading space/zero padding, emitted BEFORE the sign. */
    if (!(flags & FLAG_LEFT) && total_pad > 0 && !pad_with_zero) {
      sink_pad(s, ' ', total_pad);
    }

    sink_put(s, sign, strlen(sign));
    sink_put(s, alt, (size_t)altlen);

    /* Zero padding goes AFTER the sign and the 0x prefix: "0x-1" would be
     * nonsense, and putting the zeros first would change the value's meaning. */
    if (pad_with_zero) {
      sink_pad(s, '0', total_pad);
    }
    if (zero_digits > 0) {
      sink_pad(s, '0', zero_digits);
    }
    sink_put(s, digits, (size_t)nlen);

    if ((flags & FLAG_LEFT) && total_pad > 0) {
      sink_pad(s, ' ', total_pad);
    }
  }

  return (int)s->total;
}

/* ---- public wrappers ---------------------------------------------------- */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  sink_t s = { .buf = buf, .size = size, .used = 0, .total = 0 };
  int    n = vformat(&s, fmt, ap);
  if (size > 0) {
    buf[s.used] = '\0'; /* used is capped at size-1, so this always fits */
  }
  return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int sprintf(char *buf, const char *fmt, ...) {
  /* Unbounded: the caller guarantees the buffer is big enough. Use a very
   * large cap so sink_t never has to special-case "no NUL room". */
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, (size_t)-1, fmt, ap);
  va_end(ap);
  return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  sink_t s = { .stream = f, .total = 0 };
  int    n = vformat(&s, fmt, ap);
  fflush(f);
  return n;
}

int vprintf(const char *fmt, va_list ap) {
  return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

void perror(const char *prefix) {
  if (prefix && *prefix) {
    fputs(prefix, stderr);
    fputs(": ", stderr);
  }
  fputs(strerror(errno), stderr);
  fputc('\n', stderr);
}