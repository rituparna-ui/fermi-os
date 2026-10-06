/*
 * stdio test — exercises <stdio.h>, formatted output, and real file I/O.
 *
 * Runs as an ordinary exec'd program against the real libc.a, writing a file
 * to the FAT32 disk and reading it back, so it covers the kernel's write path
 * (cluster-chain extension, directory-entry size update) as well as libc
 * buffering. Named STDIO.ELF for FAT32 8.3.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int fails;

static void ok(int cond, const char *what) {
  checks++;
  if (cond) {
    return;
  }
  fails++;
  printf("  [STDIO] FAIL: %s\n", what);
}

#define TEST_PATH "/mnt/fat32/STDIO.TXT"

static void test_snprintf(void) {
  char buf[64];

  /* Basics. */
  ok(snprintf(buf, sizeof(buf), "%d", 42) == 2 && strcmp(buf, "42") == 0,
     "snprintf %d");
  ok(snprintf(buf, sizeof(buf), "%s", "hi") == 2 && strcmp(buf, "hi") == 0,
     "snprintf %s");
  ok(snprintf(buf, sizeof(buf), "%c", 'x') == 1 && strcmp(buf, "x") == 0,
     "snprintf %c");
  ok(snprintf(buf, sizeof(buf), "100%%") == 4 && strcmp(buf, "100%") == 0,
     "snprintf literal percent");

  /* Width and alignment. */
  ok(snprintf(buf, sizeof(buf), "%5d", 42) == 5 && strcmp(buf, "   42") == 0,
     "right-aligned width");
  ok(snprintf(buf, sizeof(buf), "%-5d|", 42) == 6 && strcmp(buf, "42   |") == 0,
     "left-aligned width");
  ok(snprintf(buf, sizeof(buf), "%05d", 42) == 5 && strcmp(buf, "00042") == 0,
     "zero-padded");
  ok(snprintf(buf, sizeof(buf), "%*d", 6, 42) == 6 && strcmp(buf, "    42") == 0,
     "* width from argument");

  /* Signs. */
  ok(snprintf(buf, sizeof(buf), "%+d", 42) == 3 && strcmp(buf, "+42") == 0,
     "explicit plus sign");
  ok(snprintf(buf, sizeof(buf), "% d", 42) == 3 && strcmp(buf, " 42") == 0,
     "space flag");
  ok(snprintf(buf, sizeof(buf), "%d", -42) == 3 && strcmp(buf, "-42") == 0,
     "negative");
  /* Zero padding must go AFTER the sign: "0-42" would misparse as a value. */
  ok(snprintf(buf, sizeof(buf), "%05d", -42) == 5 && strcmp(buf, "-0042") == 0,
     "zero padding follows sign");

  /* Bases. */
  ok(snprintf(buf, sizeof(buf), "%x", 255) == 2 && strcmp(buf, "ff") == 0,
     "hex lower");
  ok(snprintf(buf, sizeof(buf), "%X", 255) == 2 && strcmp(buf, "FF") == 0,
     "hex upper");
  ok(snprintf(buf, sizeof(buf), "%#x", 255) == 4 && strcmp(buf, "0xff") == 0,
     "hex alternate form");
  ok(snprintf(buf, sizeof(buf), "%o", 8) == 2 && strcmp(buf, "10") == 0,
     "octal");
  ok(snprintf(buf, sizeof(buf), "%u", 4000000000u) == 10 &&
         strcmp(buf, "4000000000") == 0,
     "unsigned 32-bit");

  /* Length modifiers. */
  ok(snprintf(buf, sizeof(buf), "%ld", 123456789L) == 9 &&
         strcmp(buf, "123456789") == 0,
     "long");
  ok(snprintf(buf, sizeof(buf), "%lld", -1234567890123LL) == 14 &&
         strcmp(buf, "-1234567890123") == 0,
     "long long");
  ok(snprintf(buf, sizeof(buf), "%zu", (size_t)12345) == 5 &&
         strcmp(buf, "12345") == 0,
     "size_t modifier");
  ok(snprintf(buf, sizeof(buf), "%hhd", (signed char)-5) == 2 &&
         strcmp(buf, "-5") == 0,
     "signed char modifier");

  /* LLONG_MIN: negating it as a signed value overflows, so this only passes
   * if the implementation negates in unsigned space. */
  ok(snprintf(buf, sizeof(buf), "%lld", (-9223372036854775807LL - 1)) == 20 &&
         strcmp(buf, "-9223372036854775808") == 0,
     "LLONG_MIN does not overflow");

  /* Precision on strings truncates; precision on integers zero-pads. */
  ok(snprintf(buf, sizeof(buf), "%.3s", "abcdef") == 3 &&
         strcmp(buf, "abc") == 0,
     "string precision truncates");
  ok(snprintf(buf, sizeof(buf), "%.5d", 42) == 5 && strcmp(buf, "00042") == 0,
     "integer precision pads with zeros");

  /* Truncation: the return value must be the length the FULL result needed,
   * not the truncated length. That is the whole point of snprintf's contract. */
  char small[4];
  int  n = snprintf(small, sizeof(small), "%s", "abcdefgh");
  ok(n == 8, "snprintf returns untruncated length");
  ok(strcmp(small, "abc") == 0, "snprintf truncated and NUL-terminated");

  /* NULL must not be dereferenced. */
  ok(snprintf(buf, sizeof(buf), "%s", (char *)0) == 6 &&
         strcmp(buf, "(null)") == 0,
     "NULL string renders as (null)");

  /* Mixed text and conversions in one format. */
  ok(snprintf(buf, sizeof(buf), "[%d/%s/%c]", 7, "x", 'z') == 7 &&
         strcmp(buf, "[7/x/z]") == 0,
     "mixed conversions");

  /* %* precision. */
  ok(snprintf(buf, sizeof(buf), "%.*d", 4, 7) == 4 && strcmp(buf, "0007") == 0,
     "* precision from argument");

  /* Pointer formatting must not crash and should be 0x-prefixed. */
  void *p = (void *)0x1234;
  int  m = snprintf(buf, sizeof(buf), "%p", p);
  ok(m == 6 && strncmp(buf, "0x1234", 6) == 0, "%p renders as 0x...");
}

static void test_file_io(void) {
  /* Create, write, close. This is the first user-space WRITE on this kernel,
   * so it exercises FAT32 cluster-chain extension and the directory-entry size
   * update that fat32_write_at() relies on. */
  FILE *f = fopen(TEST_PATH, "w");
  ok(f != NULL, "fopen for write (O_CREAT|O_TRUNC)");
  if (!f) {
    printf("  [STDIO]   errno=%d (%s)\n", errno, strerror(errno));
    return;
  }

  /* Write enough to cross several sectors AND the 512-byte stdio buffer, so
   * the buffer-flush path and a multi-sector write are both covered. */
  size_t total = 0;
  for (int i = 0; i < 200; i++) {
    char line[64];
    int  n = snprintf(line, sizeof(line), "line %03d: the quick brown fox\n", i);
    fwrite(line, 1, (size_t)n, f);
    total += (size_t)n;
  }
  fputs("tail marker\n", f);

  ok(ftell(f) > 0, "ftell reports a position while writing");
  ok(fclose(f) == 0, "fclose flushes and closes");

  /* Reopen and read back. */
  f = fopen(TEST_PATH, "r");
  ok(f != NULL, "fopen for read");
  if (!f) {
    return;
  }

  /* "line 000" is 8 characters. Reading 9 bytes would include the ':' and
   * fail the comparison against the NUL-terminated literal — the buffer would
   * be correct and the assertion wrong. */
  char buf[128];
  size_t got = fread(buf, 1, 8, f);
  ok(got == 8 && strncmp(buf, "line 000", 8) == 0, "fread first bytes");
  ok(ftell(f) == 8, "ftell after partial fread");

  ok(fgetc(f) == ':', "fgetc continues after the buffer");

  /* Seek back and re-read: must invalidate the buffered state correctly. */
  ok(fseek(f, 0, SEEK_SET) == 0, "fseek to start");
  ok(fgetc(f) == 'l', "read after fseek");
  /* Position is 1 (fgetc consumed 'l'), so -1 returns to the line start. */
  ok(fseek(f, -1, SEEK_CUR) == 0, "fseek relative (SEEK_CUR, negative)");
  got = fread(buf, 1, 8, f);
  ok(got == 8 && strncmp(buf, "line 000", 8) == 0, "re-read after SEEK_CUR");

  /* A relative seek that would land before the start of the file must fail
   * rather than silently clamping — clamping would make a caller looping
   * "back one, retry" never make progress. */
  errno = 0;
  ok(fseek(f, -100000, SEEK_CUR) == -1 && errno == EINVAL,
     "SEEK_CUR before start of file is rejected");

  /* Seek to the very end and confirm the tail marker survived — this is what
   * proves the on-disk size was updated, not just the in-memory one. */
  ok(fseek(f, 0, SEEK_END) == 0, "fseek to end");
  long end = ftell(f);
  ok(end == (long)(total + strlen("tail marker\n")),
     "file size on disk matches bytes written");

  ok(fseek(f, end - 12, SEEK_SET) == 0, "fseek near end");
  char tail[16];
  memset(tail, 0, sizeof(tail));
  ok(fread(tail, 1, 12, f) == 12, "read the tail");
  ok(strcmp(tail, "tail marker\n") == 0, "tail marker survived the round trip");

  /* EOF behaviour. */
  ok(fseek(f, 0, SEEK_END) == 0, "fseek to end for EOF test");
  ok(fgetc(f) == EOF, "fgetc at EOF returns EOF");
  ok(feof(f) == 1, "feof reports EOF");

  fclose(f);

  /* Unlinking is not implemented; remove() must say so rather than pretend. */
  ok(remove(TEST_PATH) == -1 && errno == EOPNOTSUPP,
     "remove() reports EOPNOTSUPP");
}

/* Mode strings and their access semantics. */
static void test_modes(void) {
  /* Opening for read a file that does not exist must fail with ENOENT. */
  FILE *f = fopen("/mnt/fat32/NOPE.TXT", "r");
  ok(f == NULL, "fopen missing file for read fails");
  ok(errno == ENOENT, "missing file sets errno to ENOENT");
  ok(f == NULL, "no stream leaked on failed open");

  /* A read-only fd must refuse writes, and vice versa. That check lives in the
   * VFS layer, so this exercises the kernel as well as libc. */
  f = fopen(TEST_PATH, "r");
  ok(f != NULL, "reopen for read");
  if (f) {
    errno = 0;
    ok(fputc('x', f) == EOF, "write to a read-only stream fails");
    ok(errno == EBADF, "read-only write sets errno to EBADF");
    fclose(f);
  }
}

static void test_ungetc(void) {
  FILE *f = fopen(TEST_PATH, "r");
  if (!f) {
    ok(0, "open for ungetc test");
    return;
  }
  int c = fgetc(f);
  ok(c == 'l', "read a char");
  ok(ungetc(c, f) == c, "ungetc pushes it back");
  ok(fgetc(f) == c, "the pushed-back char comes back");

  /* Nothing to push back at the very start of the stream. */
  ok(fseek(f, 0, SEEK_SET) == 0, "seek to start");
  ok(ungetc('z', f) == EOF, "ungetc at stream start fails cleanly");
  fclose(f);
}

int main(void) {
  printf("[STDIO TEST] === Fermi libc stdio ===\n");

  test_snprintf();
  test_file_io();
  test_modes();
  test_ungetc();

  printf("[STDIO TEST] === %d checks, %d failed ===\n", checks, fails);
  return 0;
}
