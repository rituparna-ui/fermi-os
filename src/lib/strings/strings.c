#include "strings.h"

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
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
