/*
 * ls — directory listing, built on POSIX readdir(3).
 *
 * A real user-space program: linked against libc.a, exec'd by the shell with
 * `exec /mnt/fat32/LS.ELF`. It exists because directory enumeration only counts
 * as finished once something actually consumes it -- the raw getdents64 tests
 * prove the kernel side, this proves the libc side a program would use.
 *
 * Usage: ls [-a] [path]
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ENTRIES 256

/* Entry names are collected first, then sorted. Printing in whatever order the
 * filesystem hands them back is not "wrong", but a listing that reshuffles
 * between calls is unpleasant to read and impossible to test against. */
static char *names[MAX_ENTRIES];
static char  namebuf[MAX_ENTRIES][NAME_MAX + 2];
static int   is_dir[MAX_ENTRIES];
static int   order[MAX_ENTRIES];
static int   count;

/* Compares two INDIRECTIONS through `names`. */
static int cmp_order(const void *a, const void *b) {
  return strcmp(names[*(const int *)a], names[*(const int *)b]);
}

int main(int argc, char **argv) {
  int         show_all = 0;
  const char *path      = ".";

  int i = 1;
  if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
    for (const char *f = argv[i] + 1; *f; f++) {
      switch (*f) {
        case 'a': show_all = 1; break;
        default:
          fprintf(stderr, "ls: unknown option -%c\n", *f);
          return 2;
      }
    }
    i++;
  }
  if (i < argc) {
    path = argv[i];
  }

  DIR *d = opendir(path);
  if (!d) {
    /* opendir() has already set errno; strerror() turns it into the message. */
    fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
    return 1;
  }

  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!show_all) {
      /* Without -a, hide "." and "..". They are real directory entries on every
       * filesystem, including the FAT32 volume root where this kernel
       * synthesizes them, so hiding them is readdir's job, not the filesystem's
       * -- which is exactly how every other readdir behaves. */
      if (e->d_name[0] == '.' &&
          (e->d_name[1] == '\0' ||
           (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
        continue;
      }
    }
    if (count >= MAX_ENTRIES) {
      fprintf(stderr, "ls: too many entries (limit %d)\n", MAX_ENTRIES);
      break;
    }
    /* Copy out of readdir's storage before the next call overwrites it. */
    size_t n = strlen(e->d_name);
    if (n > NAME_MAX) {
      n = NAME_MAX;
    }
    memcpy(namebuf[count], e->d_name, n);
    namebuf[count][n] = '\0';
    is_dir[count]      = (e->d_type == DT_DIR);
    names[count]       = namebuf[count];
    count++;
  }
  closedir(d);

  /* Sort an index array rather than the names themselves: `is_dir` is keyed to
   * the unsorted positions, so permuting `names` in place would silently attach
   * each entry's "/" suffix to the wrong one. */
  for (int k = 0; k < count; k++) {
    order[k] = k;
  }
  qsort(order, (size_t)count, sizeof(order[0]), cmp_order);

  for (int k = 0; k < count; k++) {
    int e = order[k];
    printf("%s%s\n", names[e], is_dir[e] ? "/" : "");
  }

  return 0;
}