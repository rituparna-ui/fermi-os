#ifndef LIB_STRINGS_H
#define LIB_STRINGS_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
const char *strchr(const char *s, int c);

#include <stdarg.h>

/* snprintf-like formatter writing to a fixed-size buffer.
 * Supported specifiers (subset of stdio):
 *   %s   const char *  (NULL prints "(null)")
 *   %d   int64_t       (signed decimal)
 *   %u   uint64_t      (unsigned decimal)
 *   %x   uint64_t      (lowercase hex, no 0x prefix)
 *   %c   char (passed as int per default-promotion)
 *   %%   literal percent
 *
 * Returns the number of bytes that would have been written excluding
 * the NUL terminator (POSIX semantics). The output is always NUL-
 * terminated when buflen > 0.
 */
int ksnprintf(char *buf, size_t buflen, const char *fmt, ...);
int kvsnprintf(char *buf, size_t buflen, const char *fmt, va_list args);

#endif
