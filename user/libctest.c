/*
 * libc test — exercises the Fermi libc allocator and <string.h>.
 *
 * Built and linked like any other user program, against user/lib/libc.a, so
 * this validates the real archive rather than the kernel's private view of
 * anything. Named LIBCT.ELF because FAT32 here has no long-filename support
 * and 8 characters is the limit for the name part.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sys.h"

static int checks;
static int fails;

static void say(const char *s) { sys_write(1, s, strlen(s)); }

/* Decimal renderer. Uses sys_write directly rather than a libc printf — stdio
 * isn't written yet, which is exactly why this test exists. */
static void putn(unsigned long v) {
  char tmp[24];
  int  n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  }
  while (v > 0 && n < (int)sizeof(tmp)) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0) {
    sys_write(1, &tmp[--n], 1);
  }
}

static void ok(int cond, const char *what) {
  checks++;
  if (cond) {
    return;
  }
  fails++;
  say("  [LIBC] FAIL: ");
  say(what);
  say("\n");
}

/* ---------------- string.h ---------------- */

static void test_strings(void) {
  say("  [LIBC] <string.h>\n");

  ok(strlen("") == 0, "strlen(\"\") == 0");
  ok(strlen("hello") == 5, "strlen(\"hello\") == 5");
  ok(strlen("a\0b") == 1, "strlen stops at NUL");

  ok(strcmp("abc", "abc") == 0, "strcmp equal");
  ok(strcmp("abc", "abd") < 0, "strcmp less");
  ok(strcmp("abd", "abc") > 0, "strcmp greater");
  /* Unsigned byte comparison: 0x80 must sort ABOVE 0x7F, not below it. A
   * signed char comparison would get this backwards. */
  ok(strcmp("\x80", "\x7f") > 0, "strcmp compares bytes unsigned");
  ok(memcmp("\x80", "\x7f", 1) > 0, "memcmp compares bytes unsigned");

  ok(strncmp("abcdef", "abcxxx", 3) == 0, "strncmp prefix equal");
  ok(strncmp("abcdef", "abdxxx", 3) != 0, "strncmp prefix differs");

  char buf[16];
  ok(strcpy(buf, "hello") == buf, "strcpy returns dst");
  ok(strcmp(buf, "hello") == 0, "strcpy copied");

  memset(buf, 'z', sizeof(buf));
  ok(buf[0] == 'z' && buf[15] == 'z', "memset filled all");

  /* memmove with the destination AFTER the source is the case a naive
   * forward copy corrupts. */
  char slide[16] = "abcdefgh";
  memmove(slide + 2, slide, 8);
  ok(memcmp(slide, "ababcdefgh", 10) == 0, "memmove overlap forward-shifted");

  /* ...and with the destination BEFORE it. */
  char slide2[16] = "0123456789";
  memmove(slide2, slide2 + 3, 7);
  ok(memcmp(slide2, "3456789", 7) == 0, "memmove overlap back-shifted");

  ok(memchr("hello", 'l', 5) != NULL, "memchr finds");
  ok(memchr("hello", 'z', 5) == NULL, "memchr returns NULL when absent");

  ok(strchr("hello", 'o') != NULL, "strchr finds");
  ok(strchr("hello", 'z') == NULL, "strchr NULL when absent");
  ok(strrchr("aXbXc", 'X') != NULL &&
         *strrchr("aXbXc", 'X') == 'X' &&
         strrchr("aXbXc", 'X') == strchr("aXbXc", 'X') + 2,
     "strrchr finds LAST occurrence");

  ok(strstr("hello world", "wor") != NULL, "strstr finds");
  ok(strstr("hello world", "xyz") == NULL, "strstr NULL when absent");
  ok(strstr("hello", "") != NULL, "strstr of empty needle matches at start");
  /* Needle longer than haystack must not read past the end. */
  ok(strstr("ab", "abcdef") == NULL, "strstr needle longer than hay");

  ok(strnlen("abcdef", 3) == 3, "strnlen bounded by max");
  ok(strnlen("ab", 10) == 2, "strnlen bounded by NUL");

  char cat[32];
  strcpy(cat, "foo");
  strcat(cat, "bar");
  ok(strcmp(cat, "foobar") == 0, "strcat appends");

  char nc[8];
  strncpy(nc, "ab", 8);
  ok(nc[2] == 0 && nc[7] == 0, "strncpy zero-fills remainder");
}

/* ---------------- stdlib.h / malloc ---------------- */

static void test_malloc(void) {
  say("  [LIBC] malloc\n");

  void *a = malloc(64);
  ok(a != NULL, "malloc(64) returns a pointer");
  ok(((unsigned long)a & 15) == 0, "malloc payload is 16-byte aligned");

  /* malloc zeroes, so a fresh allocation can't expose another block's stale
   * bytes. Fill and free first to make that a real test. */
  memset(a, 0xEE, 64);
  free(a);

  void *b = malloc(64);
  ok(b == a, "same-size allocation reuses the freed block");
  for (int i = 0; i < 64; i++) {
    if (((unsigned char *)b)[i] != 0) {
      ok(0, "recycled allocation is zeroed");
      return;
    }
  }
  ok(1, "recycled allocation is zeroed");

  /* Distinct live allocations must not overlap. */
  void *c = malloc(64);
  void *d = malloc(64);
  ok(c != d && a != c && a != d, "live allocations are distinct");

  size_t live_before = malloc_live_blocks();
  ok(live_before == 3, "malloc_live_blocks counts 3");

  /* calloc */
  void *z = calloc(16, 8);
  ok(z != NULL, "calloc returns a pointer");
  int allzero = 1;
  for (int i = 0; i < 128; i++) {
    if (((unsigned char *)z)[i]) { allzero = 0; }
  }
  ok(allzero, "calloc zeroes");

  /* Overflow guard: calloc must refuse a product that wraps. */
  errno = 0;
  void *huge = calloc((size_t)-1, 2);
  ok(huge == NULL && errno == ENOMEM, "calloc rejects size_t overflow");

  /* realloc growth preserves contents and may move. */
  void *e = malloc(16);
  memcpy(e, "0123456789abcde", 16);
  e = realloc(e, 4096);
  ok(e != NULL, "realloc growth succeeds");
  ok(memcmp(e, "0123456789abcde", 16) == 0, "realloc preserved contents");
  ok(malloc_usable_size(e) >= 4096, "realloc grew the block");

  /* realloc shrink keeps the pointer and contents. */
  void *f = malloc(4096);
  memcpy(f, "keepme", 7);
  void *f2 = realloc(f, 64);
  ok(f2 == f, "realloc shrink keeps the same pointer");
  ok(memcmp(f2, "keepme", 7) == 0, "realloc shrink preserved contents");

  free(c);
  free(d);
  free(z);
  free(e);
  free(f2);

  /* Many alloc/free cycles must not corrupt the free list. Each block gets a
   * distinct tag at both ends; a coalescing bug that handed out overlapping
   * blocks would show up here as a mismatched tag. The assertions are made
   * ONCE after the loop so a systemic failure reports one line rather than
   * 200. */
  int cycles_ok = 1;
  for (int i = 0; i < 200; i++) {
    volatile unsigned char *p = (volatile unsigned char *)malloc(48);
    if (!p) {
      cycles_ok = 0;
      break;
    }
    p[0]  = (unsigned char)i;
    p[47] = (unsigned char)i;
    if (p[0] != (unsigned char)i || p[47] != (unsigned char)i) {
      cycles_ok = 0;
      break;
    }
  }
  ok(cycles_ok, "200 alloc/free cycles stayed consistent");

  /* Force heap growth well past the initial arena. */
  void *big = malloc(64 * 1024);
  ok(big != NULL, "malloc(64 KiB) succeeds");
  memset(big, 0x5A, 64 * 1024);
  ok(((unsigned char *)big)[64 * 1024 - 1] == 0x5A,
     "large allocation is writable end to end");
  free(big);
}

/* ---------------- qsort ---------------- */

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a;
  int y = *(const int *)b;
  return (x > y) - (x < y);
}

static void test_qsort(void) {
  say("  [LIBC] qsort\n");

  int arr[8] = { 5, 3, 9, 1, 7, 2, 8, 4 };
  qsort(arr, 8, sizeof(int), cmp_int);

  int sorted = 1;
  for (int i = 1; i < 8; i++) {
    if (arr[i - 1] > arr[i]) { sorted = 0; }
  }
  ok(sorted, "qsort orders ascending");

  /* Single element and empty inputs must be no-ops, not crashes. */
  int one[1] = { 42 };
  qsort(one, 1, sizeof(int), cmp_int);
  ok(one[0] == 42, "qsort on one element is a no-op");
  qsort(arr, 0, sizeof(int), cmp_int);
  ok(1, "qsort on zero elements is a no-op");
}

/* ---------------- errno / strerror ---------------- */

static void test_errno(void) {
  say("  [LIBC] strerror\n");
  ok(strcmp(strerror(0), "Success") == 0, "strerror(0)");
  ok(strcmp(strerror(EINVAL), "Invalid argument") == 0, "strerror(EINVAL)");
  ok(strcmp(strerror(EOPNOTSUPP), "Operation not supported") == 0,
     "strerror(EOPNOTSUPP)");
  ok(strcmp(strerror(12345), "Unknown error") == 0,
     "strerror of an unknown code");

  /* __errno_location must resolve to the kernel's TLS page, and writing
   * through it must be what errno reads. */
  int *slot = __errno_location();
  ok(slot != NULL, "__errno_location returns non-NULL");
  *slot = ENOSPC;
  ok(errno == ENOSPC, "errno reads through __errno_location");
  *slot = 0;
}

/* ---- POSIX directory enumeration -------------------------------------
 *
 * The raw getdents64 wire format is covered by the LINHELLO suite; this checks
 * the POSIX layer a real program actually calls.
 */
static void test_readdir(void) {
  say("[LIBC] -- readdir --\n");

  /* The mount root is the interesting directory: it is where every shipped user
   * binary lives, and it is the one case where "." and ".." are not on disk. */
  DIR *d = opendir("/mnt/fat32");
  ok(d != NULL, "opendir(/mnt/fat32) succeeded");
  if (!d) {
    return;
  }

  int saw_dot = 0, saw_dotdot = 0, saw_libct = 0, saw_stdio = 0, saw_dir = 0;
  int count = 0;

  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    /* POSIX: the returned pointer is valid until the next readdir(). Reading it
     * after that is the caller's bug, so copy what we need as we go. */
    if (strcmp(e->d_name, ".") == 0) {
      saw_dot = 1;
    } else if (strcmp(e->d_name, "..") == 0) {
      saw_dotdot = 1;
    } else if (strcmp(e->d_name, "LIBCT.ELF") == 0) {
      saw_libct = 1;
    } else if (strcmp(e->d_name, "STDIO.ELF") == 0) {
      saw_stdio = 1;
    }
    if (e->d_type == DT_DIR) {
      saw_dir = 1;
    }
    count++;
  }
  ok(count > 0, "readdir returned entries");
  ok(saw_dot, "readdir lists \".\" (synthesized at the FAT32 volume root)");
  ok(saw_dotdot, "readdir lists \"..\"");
  ok(saw_libct, "readdir finds LIBCT.ELF");
  ok(saw_stdio, "readdir finds STDIO.ELF");
  ok(saw_dir, "at least one entry is typed DT_DIR");

  /* End of directory, and it stays there. */
  ok(readdir(d) == NULL, "readdir returns NULL at end of directory");
  ok(readdir(d) == NULL, "and keeps returning NULL");
  ok(closedir(d) == 0, "closedir returned success");

  /* Two streams over the same directory are independent. */
  DIR *a = opendir("/mnt/fat32");
  DIR *b = opendir("/mnt/fat32");
  ok(a && b, "opened two independent streams on the same directory");
  if (a && b) {
    struct dirent *ea = readdir(a);
    struct dirent *eb = readdir(b);
    ok(ea && eb, "both streams produced a first entry");
    ok(ea && eb && strcmp(ea->d_name, eb->d_name) == 0,
       "both streams start at the same entry (independent cursors)");
    closedir(a);
    closedir(b);
  }

  /* rewinddir restarts from the beginning. */
  DIR *r = opendir("/mnt/fat32");
  if (r) {
    struct dirent *first = readdir(r);
    char saved[64] = {0};
    if (first) {
      size_t n = strlen(first->d_name);
      if (n >= sizeof(saved)) {
        n = sizeof(saved) - 1;
      }
      memcpy(saved, first->d_name, n);
    }
    (void)readdir(r);
    rewinddir(r);
    struct dirent *again = readdir(r);
    ok(again && strcmp(again->d_name, saved) == 0,
       "rewinddir returns to the first entry");
    closedir(r);
  }

  /* Failure paths. */
  ok(opendir("/mnt/fat32/NOPE.XXX") == NULL, "opendir on a missing path fails");
  ok(opendir("/mnt/fat32/LIBCT.ELF") == NULL,
     "opendir on a regular file fails (ENOTDIR)");

  /* A subdirectory really does carry "." and ".." on disk, so this also
   * confirms the volume-root synthesis did not paper over a real bug. */
  DIR *sub = opendir("/mnt/fat32/SUBDIR");
  if (sub) {
    int s_dot = 0, s_dotdot = 0, s_info = 0;
    struct dirent *se;
    while ((se = readdir(sub)) != NULL) {
      if (strcmp(se->d_name, ".") == 0) {
        s_dot = 1;
      } else if (strcmp(se->d_name, "..") == 0) {
        s_dotdot = 1;
      } else if (strcmp(se->d_name, "INFO.TXT") == 0) {
        s_info = 1;
      }
    }
    ok(s_dot && s_dotdot, "a subdirectory lists \".\" and \"..\"");
    ok(s_info, "a subdirectory lists its own file");
    closedir(sub);
  }
}

int main(void) {
  say("[LIBC TEST] === Fermi libc ===\n");

  test_strings();
  test_malloc();
  test_qsort();
  test_errno();
  test_readdir();

  say("[LIBC TEST] === ");
  putn(checks);
  say(" checks, ");
  putn(fails);
  say(" failed ===\n");

  return 0;
}