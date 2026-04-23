#include "fat32.h"
#include "blk/blk.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include "utils/utils.h"

static struct {
  uint32_t fat_start_sector;
  uint32_t data_start_sector;
  uint32_t sectors_per_cluster;
  uint32_t root_cluster;
  uint32_t bytes_per_cluster;
} vol;

static uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));

static uint32_t cluster_to_sector(uint32_t cluster) {
  return vol.data_start_sector + (cluster - 2) * vol.sectors_per_cluster;
}

static uint32_t fat_next(uint32_t cluster) {
  uint32_t fat_offset = cluster * 4;
  uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
  uint32_t offset = fat_offset % SECTOR;
  if (blk_read(sector, sec_buf) != ESUCCESS)
    return FAT32_EOC;
  uint32_t val;
  memcpy(&val, sec_buf + offset, 4);
  return val & 0x0FFFFFFF;
}

/* Convert "hello.txt" -> "HELLO   TXT" (11 bytes, space-padded, uppercase). */
static void to_83(const char *name, uint8_t out[11]) {
  memset(out, ' ', 11);
  int i = 0;
  while (i < 8 && name[i] && name[i] != '.') {
    char c = name[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[i++] = (uint8_t)c;
  }
  while (name[i] && name[i] != '.')
    i++;
  if (name[i] == '.')
    i++;
  int j = 0;
  while (j < 3 && name[i]) {
    char c = name[i++];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[8 + j++] = (uint8_t)c;
  }
}

int fat32_mount(void) {
  if (blk_read(0, sec_buf) != ESUCCESS) {
    uart_errorln("[FS][FAT32] Failed to read BPB");
    return EERROR;
  }
  struct bpb b;
  memcpy(&b, sec_buf, sizeof(b));

  if (b.bytes_per_sector != SECTOR) {
    uart_errorln("[FS][FAT32] Unsupported sector size");
    return EERROR;
  }

  if (b.fat_size_16 != 0 || b.root_entries_16 != 0) {
    uart_errorln("[FS][FAT32] Not a FAT32 volume");
    return EERROR;
  }

  vol.sectors_per_cluster = b.sectors_per_cluster;
  vol.fat_start_sector = b.reserved_sectors;
  vol.data_start_sector =
      b.reserved_sectors + (uint32_t)b.num_fats * b.fat_size_32;
  vol.root_cluster = b.root_cluster;
  vol.bytes_per_cluster = (uint32_t)b.sectors_per_cluster * SECTOR;

  uart_printf(
      "[FS][FAT32] Mounted: Sectors/Cluster=%d FAT Start@%d data@%d root=%d\n",
      (uint64_t)vol.sectors_per_cluster, (uint64_t)vol.fat_start_sector,
      (uint64_t)vol.data_start_sector, (uint64_t)vol.root_cluster);
  return ESUCCESS;
}

/* Search directory starting at dir_cluster for a single 8.3 component.
 * Returns ESUCCESS and fills out_cluster, out_size, out_attr on match. */
static int dir_lookup(uint32_t dir_cluster, const uint8_t target[11],
                      uint32_t *out_cluster, uint32_t *out_size,
                      uint8_t *out_attr) {
  uint32_t cluster = dir_cluster;
  while (cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS)
        return EERROR;
      for (uint32_t off = 0; off < SECTOR; off += DIR_ENTRY_SIZE) {
        struct dir_entry *e = (struct dir_entry *)(sec_buf + off);
        if (e->name[0] == 0x00)
          return EERROR; /* end of directory */
        if (e->name[0] == 0xE5)
          continue;
        if (e->attr == ATTR_LFN)
          continue;
        if (e->attr & ATTR_VOLUME_ID)
          continue;

        int match = 1;
        for (int i = 0; i < 11; i++) {
          if (e->name[i] != target[i]) {
            match = 0;
            break;
          }
        }
        if (match) {
          *out_cluster =
              ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
          *out_size = e->size;
          *out_attr = e->attr;
          return ESUCCESS;
        }
      }
    }
    cluster = fat_next(cluster);
  }
  return EERROR;
}

int fat32_find(const char *path, uint32_t *out_first_cluster,
               uint32_t *out_size) {
  /* Skip leading slash */
  while (*path == '/')
    path++;

  uint32_t cur_cluster = vol.root_cluster;

  while (*path) {
    /* Extract next component (up to '/' or end) */
    const char *seg = path;
    while (*path && *path != '/')
      path++;
    int seg_len = (int)(path - seg);
    while (*path == '/')
      path++;

    /* Build a null-terminated component for to_83 */
    char component[13];
    if (seg_len > 12)
      seg_len = 12;
    for (int i = 0; i < seg_len; i++)
      component[i] = seg[i];
    component[seg_len] = '\0';

    uint8_t target[11];
    to_83(component, target);

    uint32_t cluster, size;
    uint8_t attr;
    if (dir_lookup(cur_cluster, target, &cluster, &size, &attr) != ESUCCESS)
      return EERROR;

    if (*path) {
      /* More components remain — this must be a directory */
      if (!(attr & ATTR_DIRECTORY))
        return EERROR;
      cur_cluster = cluster;
    } else {
      /* Final component */
      *out_first_cluster = cluster;
      *out_size = size;
      return ESUCCESS;
    }
  }

  return EERROR;
}

int fat32_read(uint32_t first_cluster, uint32_t size, void *buf,
               uint32_t buf_len) {
  if (size > buf_len)
    size = buf_len;

  uint8_t *out = (uint8_t *)buf;
  uint32_t remaining = size;
  uint32_t cluster = first_cluster;

  while (remaining > 0 && cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster && remaining > 0; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS)
        return -1;
      uint32_t chunk = remaining < SECTOR ? remaining : SECTOR;
      memcpy(out, sec_buf, chunk);
      out += chunk;
      remaining -= chunk;
    }
    cluster = fat_next(cluster);
  }
  return (int)(size - remaining);
}
