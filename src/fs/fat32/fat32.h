#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <stddef.h>
#include <stdint.h>

#define SECTOR 512
#define DIR_ENTRY_SIZE 32
#define ATTR_LFN 0x0F
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define FAT32_EOC 0x0FFFFFF8 /* any value >= this is end-of-chain */

struct bpb {
  uint8_t jmp[3];
  uint8_t oem[8];
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t num_fats;
  uint16_t root_entries_16; /* 0 on FAT32 */
  uint16_t total_sectors_16;
  uint8_t media;
  uint16_t fat_size_16; /* 0 on FAT32 */
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  /* FAT32-specific */
  uint32_t fat_size_32;
  uint16_t ext_flags;
  uint16_t fs_version;
  uint32_t root_cluster;
} __attribute__((packed));

struct dir_entry {
  uint8_t name[11]; /* 8.3 padded with spaces */
  uint8_t attr;
  uint8_t nt_res;
  uint8_t ctime_tenth;
  uint16_t ctime;
  uint16_t cdate;
  uint16_t adate;
  uint16_t first_cluster_hi;
  uint16_t wtime;
  uint16_t wdate;
  uint16_t first_cluster_lo;
  uint32_t size;
} __attribute__((packed));

int fat32_mount(void);

/* Look up a file by path (supports subdirectories, e.g. "DOCS/README.TXT") */
int fat32_find(const char *path, uint32_t *out_first_cluster,
               uint32_t *out_size);

/* Returns bytes read, or -1 on error. */
int fat32_read(uint32_t first_cluster, uint32_t size, void *buf,
               uint32_t buf_len);

/* Write `len` bytes at byte `offset`, extending the cluster chain as needed.
 * Returns ESUCCESS/EERROR. On success *out_new_size (if non-NULL) receives
 * offset + len, which the caller must store back into the directory entry:
 * in FAT32 a file's size lives in the entry, not in the cluster chain. */
int fat32_write_at(uint32_t first_cluster, uint32_t offset, const void *data,
                   uint32_t len, uint32_t *out_new_size);

/* Set a file's recorded size (for O_TRUNC). Does not free clusters — the data
 * simply becomes unreachable past the new size, which is what truncate means
 * here. Returns ESUCCESS/EERROR. */
int fat32_truncate_path(const char *path, uint32_t new_size);

/* Create a new file at path with given data. Supports subdirectories. */
int fat32_create(const char *path, const void *data, uint32_t len);

/* Look up a single 8.3 name inside a directory starting at dir_cluster.
 * On success: fills first_cluster, size, and is_dir. */
int fat32_lookup_in_dir(uint32_t dir_cluster, const char *name,
                        uint32_t *out_first_cluster, uint32_t *out_size,
                        int *out_is_dir);

/* Expose the mounted volume's root cluster (for VFS mount) */
uint32_t fat32_root_cluster(void);

/* Attach FAT32 to the VFS at `path` (path must be an existing empty dir). */
int fat32_vfs_mount(const char *path);

#endif

/* One entry produced by fat32_dir_iterate. `name` is the 8.3 short name in
 * "NAME.EXT" form with padding spaces trimmed, matching what a POSIX caller
 * expects to see from readdir. */
typedef struct {
  char     name[13];
  uint8_t  name_len;
  uint32_t size;
  uint32_t first_cluster;
  uint8_t  attr;
  uint8_t  is_dir;
} fat32_dirent_t;

/* Resumable directory iterator.
 *
 * `*io_offset` counts 32-byte directory slots already examined and is updated
 * on return. Keeping the position in a caller-supplied word rather than in
 * hidden state is what lets getdents64() resume: Linux's d_off field is exactly
 * this counter, so a program that reads a directory in several small buffers
 * must be able to come back and continue where it stopped.
 *
 * Returns ESUCCESS with *out filled, or EERROR once the directory is
 * exhausted. Sectors already fully consumed are skipped without any disk I/O.
 *
 * "." and ".." are yielded like any other entry. They are real directory slots
 * and a POSIX caller is entitled to see them; filtering them here would make
 * this diverge from every other filesystem's readdir. */
int fat32_dir_iterate(uint32_t dir_cluster, uint64_t *io_offset,
                      fat32_dirent_t *out);
