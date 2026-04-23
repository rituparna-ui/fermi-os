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

/* Write a 32-bit FAT entry for the given cluster. */
static int fat_write(uint32_t cluster, uint32_t value) {
  uint32_t fat_offset = cluster * 4;
  uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
  uint32_t offset = fat_offset % SECTOR;
  if (blk_read(sector, sec_buf) != ESUCCESS)
    return EERROR;
  /* Preserve upper 4 bits of existing entry */
  uint32_t existing;
  memcpy(&existing, sec_buf + offset, 4);
  value = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
  memcpy(sec_buf + offset, &value, 4);
  if (blk_write(sector, sec_buf) != ESUCCESS)
    return EERROR;
  return ESUCCESS;
}

/* Scan FAT for a free cluster (entry == 0). Returns 0 on failure. */
static uint32_t fat_alloc_cluster(void) {
  /* Cluster 0 and 1 are reserved, start scanning from 2 */
  uint32_t total_sectors =
      vol.data_start_sector; /* rough upper bound for FAT scan */
  for (uint32_t c = 2;; c++) {
    uint32_t fat_offset = c * 4;
    uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
    if (sector >= vol.data_start_sector)
      break; /* past end of FAT */
    uint32_t offset = fat_offset % SECTOR;
    if (blk_read(sector, sec_buf) != ESUCCESS)
      return 0;
    uint32_t val;
    memcpy(&val, sec_buf + offset, 4);
    if ((val & 0x0FFFFFFF) == 0) {
      /* Mark as end-of-chain */
      if (fat_write(c, 0x0FFFFFFF) != ESUCCESS)
        return 0;
      return c;
    }
  }
  (void)total_sectors;
  return 0;
}

/* Find a free directory entry slot (name[0]==0x00 or 0xE5) in the directory
 * starting at dir_cluster. Writes the entry and flushes the sector. */
static int dir_add_entry(uint32_t dir_cluster, const struct dir_entry *entry) {
  uint32_t cluster = dir_cluster;
  while (cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS)
        return EERROR;
      for (uint32_t off = 0; off < SECTOR; off += DIR_ENTRY_SIZE) {
        uint8_t first = sec_buf[off];
        if (first == 0x00 || first == 0xE5) {
          memcpy(sec_buf + off, entry, DIR_ENTRY_SIZE);
          if (blk_write(base + s, sec_buf) != ESUCCESS)
            return EERROR;
          return ESUCCESS;
        }
      }
    }
    cluster = fat_next(cluster);
  }
  return EERROR;
}

/* Write data to a cluster chain, one sector at a time. */
static int cluster_write_data(uint32_t first_cluster, const void *data,
                              uint32_t len) {
  const uint8_t *src = (const uint8_t *)data;
  uint32_t remaining = len;
  uint32_t cluster = first_cluster;

  while (remaining > 0 && cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster && remaining > 0; s++) {
      /* If less than a full sector, read-modify-write */
      if (remaining < SECTOR) {
        memset(sec_buf, 0, SECTOR);
        memcpy(sec_buf, src, remaining);
      } else {
        memcpy(sec_buf, src, SECTOR);
      }
      if (blk_write(base + s, sec_buf) != ESUCCESS)
        return EERROR;
      uint32_t chunk = remaining < SECTOR ? remaining : SECTOR;
      src += chunk;
      remaining -= chunk;
    }
    cluster = fat_next(cluster);
  }
  return ESUCCESS;
}

int fat32_create(const char *path, const void *data, uint32_t len) {
  /* Split path into parent directory and filename */
  const char *p = path;
  while (*p == '/')
    p++;

  /* Find last '/' to separate dir from filename */
  const char *last_slash = 0;
  for (const char *s = p; *s; s++) {
    if (*s == '/')
      last_slash = s;
  }

  uint32_t dir_cluster = vol.root_cluster;

  char filename[13];
  if (last_slash) {
    /* Walk to parent directory */
    const char *walk = p;
    while (walk < last_slash) {
      const char *seg = walk;
      while (walk < last_slash && *walk != '/')
        walk++;

      int seg_len = (int)(walk - seg);
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
      if (dir_lookup(dir_cluster, target, &cluster, &size, &attr) != ESUCCESS)
        return EERROR;
      if (!(attr & ATTR_DIRECTORY))
        return EERROR;
      dir_cluster = cluster;

      while (walk <= last_slash && *walk == '/')
        walk++;
    }
    /* Filename is after last slash */
    const char *fname = last_slash + 1;
    int i = 0;
    while (fname[i] && i < 12) {
      filename[i] = fname[i];
      i++;
    }
    filename[i] = '\0';
  } else {
    /* No directory component — file goes in root */
    int i = 0;
    while (p[i] && i < 12) {
      filename[i] = p[i];
      i++;
    }
    filename[i] = '\0';
  }

  /* Allocate clusters for data */
  uint32_t clusters_needed =
      len == 0 ? 0 : (len + vol.bytes_per_cluster - 1) / vol.bytes_per_cluster;

  uint32_t first_cluster = 0;
  uint32_t prev_cluster = 0;

  for (uint32_t i = 0; i < clusters_needed; i++) {
    uint32_t c = fat_alloc_cluster();
    if (c == 0)
      return EERROR;
    if (i == 0)
      first_cluster = c;
    if (prev_cluster) {
      /* Chain previous cluster to this one */
      if (fat_write(prev_cluster, c) != ESUCCESS)
        return EERROR;
    }
    prev_cluster = c;
  }

  /* Write file data */
  if (len > 0 && first_cluster) {
    if (cluster_write_data(first_cluster, data, len) != ESUCCESS)
      return EERROR;
  }

  /* Build directory entry */
  struct dir_entry de;
  memset(&de, 0, sizeof(de));

  uint8_t name83[11];
  to_83(filename, name83);
  memcpy(de.name, name83, 11);

  de.attr = 0x20; /* ATTR_ARCHIVE */
  de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
  de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
  de.size = len;

  if (dir_add_entry(dir_cluster, &de) != ESUCCESS)
    return EERROR;

  return ESUCCESS;
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
