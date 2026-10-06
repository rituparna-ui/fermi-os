#include <string.h>

/*
 * <string.h> implementation.
 *
 * Everything here is byte-at-a-time on purpose. At -O0, GCC emits a call to
 * memcpy for struct assignment and array initialization, so memcpy must exist
 * and must be correct before anything else can be trusted — a word-at-a-time
 * version would risk the compiler turning a buggy loop into a buggy wider
 * load.
 *
 * All of these are declared without restrict. The kernel passes user-supplied
 * (and therefore possibly overlapping) buffers straight through from syscalls,
 * so being permissive costs nothing and being wrong here is a silent data
 * corruption bug in a kernel.
 */

void *memcpy(void *dst, const void *src, size_t n) {
  /* Overlap is undefined behaviour for memcpy, but callers in this tree do
   * pass overlapping ranges (kernel->user buffer copies in particular), so
   * delegate to memmove. Being stricter than required here costs nothing and
   * removes a whole class of "works until it doesn't" bug. */
  return memmove(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char       *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;

  if (d == s || n == 0) {
    return dst;
  }

  if (d < s) {
    /* Copy forward: destination starts before source, so reading s[i] before
     * it can be clobbered by a previous write is impossible. */
    for (size_t i = 0; i < n; i++) {
      d[i] = s[i];
    }
  } else {
    /* Copy backward: destination starts after source, so a forward copy would
     * overwrite the next source byte before reading it. This is the whole
     * reason memmove exists as a separate function. */
    for (size_t i = n; i > 0; i--) {
      d[i - 1] = s[i - 1];
    }
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  unsigned char  v = (unsigned char)c;
  while (n--) {
    *d++ = v;
  }
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;

  for (size_t i = 0; i < n; i++) {
    if (p[i] != q[i]) {
      /* Unsigned compare: memcmp must order by unsigned byte value, not by
       * signed char. Using signed comparison breaks every test that compares
       * bytes >= 0x80. */
      return (int)p[i] - (int)q[i];
    }
  }
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char        v = (unsigned char)c;

  for (size_t i = 0; i < n; i++) {
    if (p[i] == v) {
      return (void *)&p[i];
    }
  }
  return NULL;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p) {
    p++;
  }
  return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) {
    n++;
  }
  return n;
}

int strcmp(const char *a, const char *b) {
  /* Compare as unsigned char, matching memcmp and strcoll: bytes >= 0x80 must
   * not compare as negative. */
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;

  while (*p && *p == *q) {
    p++;
    q++;
  }
  return (int)*p - (int)*q;
}

int strncmp(const char *a, const char *b, size_t n) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;

  while (n && *p && *p == *q) {
    p++;
    q++;
    n--;
  }
  if (n == 0) {
    return 0;
  }
  return (int)*p - (int)*q;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++) != '\0') {
  }
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++) {
    dst[i] = src[i];
  }
  /* Zero-fill the remainder, as POSIX requires. */
  for (; i < n; i++) {
    dst[i] = '\0';
  }
  return dst;
}

char *strcat(char *dst, const char *src) {
  char *d = dst;
  while (*d) {
    d++;
  }
  while ((*d++ = *src++) != '\0') {
  }
  return dst;
}

char *strchr(const char *s, int c) {
  char        v = (char)c;
  const char *p = s;

  for (;;) {
    if (*p == v) {
      return (char *)p;
    }
    if (*p == '\0') {
      return NULL;
    }
    p++;
  }
}

char *strrchr(const char *s, int c) {
  char        v = (char)c;
  const char *last = NULL;
  const char *p = s;

  for (;;) {
    if (*p == v) {
      last = p;
    }
    if (*p == '\0') {
      return (char *)last;
    }
    p++;
  }
}

char *strstr(const char *hay, const char *needle) {
  if (*needle == '\0') {
    return (char *)hay;
  }

  size_t nlen = strlen(needle);
  /* Bail out early rather than reading past the end of hay: if the remaining
   * haystack is shorter than the needle there is no match. */
  size_t hlen = strlen(hay);
  if (nlen > hlen) {
    return NULL;
  }

  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (hay[i] == needle[0] && strncmp(hay + i, needle, nlen) == 0) {
      return (char *)&hay[i];
    }
  }
  return NULL;
}