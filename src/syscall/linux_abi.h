#ifndef SYSCALL_LINUX_ABI_H
#define SYSCALL_LINUX_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Linux UAPI structures, as the kernel must present them to a Linux binary.
 *
 * These are NOT the same as our own libc's types, and guessing is not an option:
 * a foreign binary's musl or glibc declares `struct stat` in its own header and
 * passes a pointer to it, so if this layout is off by even one field the caller
 * reads a size out of the wrong slot and gets plausible garbage. Every offset is
 * asserted below against the layout these definitions actually produce, and the
 * whole arrangement was taken from the toolchain's own asm-generic/stat.h rather
 * than from memory.
 */

/* struct timespec. Identical on every Linux arch with 64-bit long. */
struct linux_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

/*
 * struct stat, 128 bytes -- the generic 64-bit ABI layout.
 *
 * Note st_blksize is a 4-byte int followed by a 4-byte pad, not a 64-bit value,
 * and the three timestamps are pairs of 64-bit (sec, nsec) rather than nested
 * structs. Both are the kind of detail that looks like a typo and is not.
 */
struct linux_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t  st_size;
  int32_t  st_blksize;
  int32_t  __pad2;
  int64_t  st_blocks;      /* in 512-byte units, per the kernel's definition */
  int64_t  st_atime;
  uint64_t st_atime_nsec;
  int64_t  st_mtime;
  uint64_t st_mtime_nsec;
  int64_t  st_ctime;
  uint64_t st_ctime_nsec;
  uint32_t __unused4;
  uint32_t __unused5;
};

/*
 * struct linux_dirent64.
 *
 * The name is NOT NUL-terminated in the buffer: its length comes from d_reclen
 * covering the fixed header plus the name, and a NUL is included in that length
 * so the last entry in a buffer is still safe to treat as a C string. d_reclen
 * is padded so the next entry stays 8-byte aligned.
 */
struct linux_dirent64 {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[]; /* offset 19 */
};

#define LNX_DIRENT64_NAME_OFFSET 19
#define LNX_DT_UNKNOWN 0
#define LNX_DT_FIFO    1
#define LNX_DT_CHR     2
#define LNX_DT_DIR     4
#define LNX_DT_BLK     6
#define LNX_DT_REG     8

/* File type bits, as they appear in st_mode. */
#define LNX_S_IFMT   0170000
#define LNX_S_IFREG  0100000
#define LNX_S_IFDIR  0040000
#define LNX_S_IFCHR  0020000
#define LNX_S_IFBLK  0060000
#define LNX_S_IFIFO  0010000

/* Permission bits. Every file this kernel has is world-read/write, matching
 * the fixed 0644 that open() already uses. */
#define LNX_S_IRUSR  00400
#define LNX_S_IWUSR  00200
#define LNX_S_IXUSR  00100
#define LNX_S_IRGRP  00040
#define LNX_S_IWGRP  00020
#define LNX_S_IXGRP  00010
#define LNX_S_IROTH  00004
#define LNX_S_IWOTH  00002
#define LNX_S_IXOTH  00001

/* clockid_t values we accept. Anything else is refused rather than silently
 * treated as one of these: a caller asking for a different clock would get
 * numbers from the wrong clock and have no way to tell. */
#define LNX_CLOCK_REALTIME  0
#define LNX_CLOCK_MONOTONIC 1

/* The offsets a foreign libc's <sys/stat.h> is compiled against. If a field is
 * reordered or retyped above, these fail the build instead of producing a
 * struct that silently reads the wrong bytes at runtime. */
_Static_assert(sizeof(struct linux_stat) == 128, "linux_stat must be 128 bytes");
_Static_assert(offsetof(struct linux_stat, st_mode)   == 16, "st_mode offset");
_Static_assert(offsetof(struct linux_stat, st_nlink)  == 20, "st_nlink offset");
_Static_assert(offsetof(struct linux_stat, st_uid)    == 24, "st_uid offset");
_Static_assert(offsetof(struct linux_stat, st_gid)    == 28, "st_gid offset");
_Static_assert(offsetof(struct linux_stat, st_rdev)   == 32, "st_rdev offset");
_Static_assert(offsetof(struct linux_stat, st_size)   == 48, "st_size offset");
_Static_assert(offsetof(struct linux_stat, st_blksize)== 56, "st_blksize offset");
_Static_assert(offsetof(struct linux_stat, st_blocks) == 64, "st_blocks offset");
_Static_assert(offsetof(struct linux_stat, st_atime)  == 72, "st_atime offset");
_Static_assert(offsetof(struct linux_stat, st_mtime)  == 88, "st_mtime offset");
_Static_assert(offsetof(struct linux_stat, st_ctime)  == 104, "st_ctime offset");

/* d_name must sit at offset 19: 8 + 8 + 2 + 1. */
_Static_assert(offsetof(struct linux_dirent64, d_name) == LNX_DIRENT64_NAME_OFFSET,
               "linux_dirent64 d_name must be at offset 19");
/* NOTE: sizeof() this struct is 24, not 19 -- the flexible array leaves the
 * struct padded out to its 8-byte alignment. offsetof(d_name) is the number that
 * defines the wire layout, so that is what gets asserted. */

_Static_assert(sizeof(struct linux_timespec) == 16, "linux_timespec must be 16 bytes");
_Static_assert(offsetof(struct linux_timespec, tv_sec)  == 0, "tv_sec offset");
_Static_assert(offsetof(struct linux_timespec, tv_nsec) == 8, "tv_nsec offset");

#endif