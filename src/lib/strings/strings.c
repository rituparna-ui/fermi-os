#include "strings.h"

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
  }

  return dest;
}

// Like memcpy but safe for overlapping buffers.
void *memmove(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (d == s || n == 0) {
    return dest;
  }
  if (d < s) {
    for (size_t i = 0; i < n; i++) {
      d[i] = s[i];
    }
  } else {
    for (size_t i = n; i > 0; i--) {
      d[i - 1] = s[i - 1];
    }
  }
  return dest;
}

void *memset(void *dest, int c, size_t n) {
  uint8_t *d = (uint8_t *)dest;

  for (size_t i = 0; i < n; i++) {
    d[i] = (uint8_t)c;
  }

  return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *pa = (const uint8_t *)a;
  const uint8_t *pb = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) {
    if (pa[i] != pb[i]) {
      return (int)pa[i] - (int)pb[i];
    }
  }
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) {
    n++;
  }
  return n;
}

size_t strnlen(const char *s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n]) {
    n++;
  }
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t ca = (uint8_t)a[i];
    uint8_t cb = (uint8_t)b[i];
    if (ca != cb) {
      return (int)ca - (int)cb;
    }
    if (ca == 0) {
      return 0;
    }
  }
  return 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i]; i++) {
    dest[i] = src[i];
  }
  // POSIX: pad remainder with NULs (so dest is always NUL-terminated if
  // strlen(src) < n)
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

/* Internal: write one char to buf if there's room (always advances pos). */
static inline void buf_putc(char *buf, size_t buflen, size_t *pos, char c) {
  if (*pos + 1 < buflen) {
    buf[*pos] = c;
  }
  (*pos)++;
}

static void buf_puts(char *buf, size_t buflen, size_t *pos, const char *s) {
  while (*s) {
    buf_putc(buf, buflen, pos, *s++);
  }
}

static void buf_putu(char *buf, size_t buflen, size_t *pos, uint64_t val,
                     int base) {
  static const char digits[] = "0123456789abcdef";
  char tmp[24];
  int i = 0;
  if (val == 0) {
    buf_putc(buf, buflen, pos, '0');
    return;
  }
  while (val > 0) {
    tmp[i++] = digits[val % (uint64_t)base];
    val /= (uint64_t)base;
  }
  while (i--) {
    buf_putc(buf, buflen, pos, tmp[i]);
  }
}

int kvsnprintf(char *buf, size_t buflen, const char *fmt, va_list args) {
  size_t pos = 0;

  while (*fmt) {
    if (*fmt != '%') {
      buf_putc(buf, buflen, &pos, *fmt++);
      continue;
    }
    fmt++;

    switch (*fmt) {
    case '\0':
      goto done; /* lone trailing % */
    case 's': {
      const char *s = va_arg(args, const char *);
      buf_puts(buf, buflen, &pos, s ? s : "(null)");
      break;
    }
    case 'd': {
      int val = va_arg(args, int);
      if (val < 0) {
        buf_putc(buf, buflen, &pos, '-');
        /* avoid INT_MIN overflow via unsigned arithmetic */
        buf_putu(buf, buflen, &pos, (uint64_t)(0 - (uint64_t)val), 10);
      } else {
        buf_putu(buf, buflen, &pos, (uint64_t)val, 10);
      }
      break;
    }
    case 'u': {
      uint64_t val = va_arg(args, uint64_t);
      buf_putu(buf, buflen, &pos, val, 10);
      break;
    }
    case 'x': {
      uint64_t val = va_arg(args, uint64_t);
      buf_putu(buf, buflen, &pos, val, 16);
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      buf_putc(buf, buflen, &pos, c);
      break;
    }
    case '%':
      buf_putc(buf, buflen, &pos, '%');
      break;
    default:
      /* Unknown specifier: pass through literally. */
      buf_putc(buf, buflen, &pos, '%');
      buf_putc(buf, buflen, &pos, *fmt);
      break;
    }
    fmt++;
  }
done:
  if (buflen > 0) {
    buf[(pos < buflen) ? pos : buflen - 1] = '\0';
  }
  return (int)pos;
}

int ksnprintf(char *buf, size_t buflen, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = kvsnprintf(buf, buflen, fmt, args);
  va_end(args);
  return n;
}


const char *strchr(const char *s, int c) {
  unsigned char target = (unsigned char)c;
  for (; *s; s++) {
    if ((unsigned char)*s == target) {
      return s;
    }
  }
  return target == 0 ? s : 0;
}
