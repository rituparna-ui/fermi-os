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

/* Create a new file at path with given data. Supports subdirectories. */
int fat32_create(const char *path, const void *data, uint32_t len);

#endif
