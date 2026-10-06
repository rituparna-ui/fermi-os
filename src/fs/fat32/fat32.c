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


static uint32_t cluster_to_sector(uint32_t cluster) {
  return vol.data_start_sector + (cluster - 2) * vol.sectors_per_cluster;
}

static uint32_t fat_next(uint32_t cluster) {
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  uint32_t fat_offset = cluster * 4;
  uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
  uint32_t offset = fat_offset % SECTOR;
  if (blk_read(sector, sec_buf) != ESUCCESS) {
    return FAT32_EOC;
  }
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
    if (c >= 'a' && c <= 'z') {
      c -= 32;
    }
    out[i++] = (uint8_t)c;
  }
  while (name[i] && name[i] != '.') {
    i++;
  }
  if (name[i] == '.') {
    i++;
  }
  int j = 0;
  while (j < 3 && name[i]) {
    char c = name[i++];
    if (c >= 'a' && c <= 'z') {
      c -= 32;
    }
    out[8 + j++] = (uint8_t)c;
  }
}

int fat32_mount(void) {
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
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
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  uint32_t cluster = dir_cluster;
  while (cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS) {
        return EERROR;
      }
      for (uint32_t off = 0; off < SECTOR; off += DIR_ENTRY_SIZE) {
        struct dir_entry *e = (struct dir_entry *)(sec_buf + off);
        if (e->name[0] == 0x00) {
          return EERROR; /* end of directory */
        }
        if (e->name[0] == 0xE5) {
          continue;
        }
        if (e->attr == ATTR_LFN) {
          continue;
        }
        if (e->attr & ATTR_VOLUME_ID) {
          continue;
        }

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
  while (*path == '/') {
    path++;
  }

  uint32_t cur_cluster = vol.root_cluster;

  while (*path) {
    /* Extract next component (up to '/' or end) */
    const char *seg = path;
    while (*path && *path != '/') {
      path++;
    }
    int seg_len = (int)(path - seg);
    while (*path == '/') {
      path++;
    }

    /* Build a null-terminated component for to_83 */
    char component[13];
    if (seg_len > 12) {
      seg_len = 12;
    }
    for (int i = 0; i < seg_len; i++) {
      component[i] = seg[i];
    }
    component[seg_len] = '\0';

    uint8_t target[11];
    to_83(component, target);

    uint32_t cluster, size;
    uint8_t attr;
    if (dir_lookup(cur_cluster, target, &cluster, &size, &attr) != ESUCCESS) {
      return EERROR;
    }

    if (*path) {
      /* More components remain — this must be a directory */
      if (!(attr & ATTR_DIRECTORY)) {
        return EERROR;
      }
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
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  uint32_t fat_offset = cluster * 4;
  uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
  uint32_t offset = fat_offset % SECTOR;
  if (blk_read(sector, sec_buf) != ESUCCESS) {
    return EERROR;
  }
  /* Preserve upper 4 bits of existing entry */
  uint32_t existing;
  memcpy(&existing, sec_buf + offset, 4);
  value = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
  memcpy(sec_buf + offset, &value, 4);
  if (blk_write(sector, sec_buf) != ESUCCESS) {
    return EERROR;
  }
  return ESUCCESS;
}

/* Scan FAT for a free cluster (entry == 0). Returns 0 on failure. */
static uint32_t fat_alloc_cluster(void) {
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  /* Cluster 0 and 1 are reserved, start scanning from 2 */
  uint32_t total_sectors =
      vol.data_start_sector; /* rough upper bound for FAT scan */
  for (uint32_t c = 2;; c++) {
    uint32_t fat_offset = c * 4;
    uint32_t sector = vol.fat_start_sector + fat_offset / SECTOR;
    if (sector >= vol.data_start_sector) {
      break; /* past end of FAT */
    }
    uint32_t offset = fat_offset % SECTOR;
    if (blk_read(sector, sec_buf) != ESUCCESS) {
      return 0;
    }
    uint32_t val;
    memcpy(&val, sec_buf + offset, 4);
    if ((val & 0x0FFFFFFF) == 0) {
      /* Mark as end-of-chain */
      if (fat_write(c, 0x0FFFFFFF) != ESUCCESS) {
        return 0;
      }
      return c;
    }
  }
  (void)total_sectors;
  return 0;
}

/* Find a free directory entry slot (name[0]==0x00 or 0xE5) in the directory
 * starting at dir_cluster. Writes the entry and flushes the sector. */
static int dir_add_entry(uint32_t dir_cluster, const struct dir_entry *entry) {
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  uint32_t cluster = dir_cluster;
  while (cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS) {
        return EERROR;
      }
      for (uint32_t off = 0; off < SECTOR; off += DIR_ENTRY_SIZE) {
        uint8_t first = sec_buf[off];
        if (first == 0x00 || first == 0xE5) {
          memcpy(sec_buf + off, entry, DIR_ENTRY_SIZE);
          if (blk_write(base + s, sec_buf) != ESUCCESS) {
            return EERROR;
          }
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
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
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
      if (blk_write(base + s, sec_buf) != ESUCCESS) {
        return EERROR;
      }
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
  while (*p == '/') {
    p++;
  }

  /* Find last '/' to separate dir from filename */
  const char *last_slash = 0;
  for (const char *s = p; *s; s++) {
    if (*s == '/') {
      last_slash = s;
    }
  }

  uint32_t dir_cluster = vol.root_cluster;

  char filename[13];
  if (last_slash) {
    /* Walk to parent directory */
    const char *walk = p;
    while (walk < last_slash) {
      const char *seg = walk;
      while (walk < last_slash && *walk != '/') {
        walk++;
      }

      int seg_len = (int)(walk - seg);
      char component[13];
      if (seg_len > 12) {
        seg_len = 12;
      }
      for (int i = 0; i < seg_len; i++) {
        component[i] = seg[i];
      }
      component[seg_len] = '\0';

      uint8_t target[11];
      to_83(component, target);

      uint32_t cluster, size;
      uint8_t attr;
      if (dir_lookup(dir_cluster, target, &cluster, &size, &attr) != ESUCCESS) {
        return EERROR;
      }
      if (!(attr & ATTR_DIRECTORY)) {
        return EERROR;
      }
      dir_cluster = cluster;

      while (walk <= last_slash && *walk == '/') {
        walk++;
      }
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
  /* Always allocate at least one cluster, even for an empty file.
   *
   * fat32_write_at() addresses data through the chain starting at
   * first_cluster and has no way to bootstrap a chain that doesn't exist yet,
   * so a zero-cluster file could be created but never written — the first
   * fwrite() to a freshly fopen'd "w" file would fail. Allocating one cluster
   * up front (what Linux does too) keeps the invariant "a regular file always
   * has a start cluster", and costs one cluster for an empty file. */
  uint32_t clusters_needed =
      len == 0 ? 1 : (len + vol.bytes_per_cluster - 1) / vol.bytes_per_cluster;

  uint32_t first_cluster = 0;
  uint32_t prev_cluster = 0;

  for (uint32_t i = 0; i < clusters_needed; i++) {
    uint32_t c = fat_alloc_cluster();
    if (c == 0) {
      return EERROR;
    }
    if (i == 0) {
      first_cluster = c;
    }
    if (prev_cluster) {
      /* Chain previous cluster to this one */
      if (fat_write(prev_cluster, c) != ESUCCESS) {
        return EERROR;
      }
    }
    prev_cluster = c;
  }

  /* Write file data */
  if (len > 0 && first_cluster) {
    if (cluster_write_data(first_cluster, data, len) != ESUCCESS) {
      return EERROR;
    }
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

  if (dir_add_entry(dir_cluster, &de) != ESUCCESS) {
    return EERROR;
  }

  return ESUCCESS;
}

/* Rewrite a directory entry's size field, found by walking `path`.
 *
 * FAT32 keeps a file's length in the directory entry, not in the cluster chain,
 * so extending a file on disk is not enough — the entry must be updated too or
 * the extra bytes are invisible to the next reader. Clusters are deliberately
 * NOT freed here: the data past the new size simply becomes unreachable, which
 * is exactly what truncate means, and reclaiming them needs a chain rewrite
 * with its own failure modes.
 */
int fat32_truncate_path(const char *path, uint32_t new_size) {
  /* Per-call sector buffer — see fat32_read(). */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));

  while (*path == '/') {
    path++;
  }

  /* Split into parent directory path + final component, mirroring
   * fat32_create() so both agree on what "path" means. */
  const char *slash = 0;
  for (const char *q = path; *q; q++) {
    if (*q == '/') {
      slash = q;
    }
  }

  char dirpath[256];
  char namebuf[13];

  if (slash) {
    size_t n = (size_t)(slash - path);
    if (n >= sizeof(dirpath)) {
      return EERROR;
    }
    memcpy(dirpath, path, n);
    dirpath[n] = '\0';
    const char *base = slash + 1;
    size_t bl = strlen(base);
    if (bl >= sizeof(namebuf)) {
      return EERROR;
    }
    memcpy(namebuf, base, bl + 1);
  } else {
    dirpath[0] = '\0';
    size_t bl = strlen(path);
    if (bl >= sizeof(namebuf)) {
      return EERROR;
    }
    memcpy(namebuf, path, bl + 1);
  }

  uint32_t dir_cluster = vol.root_cluster;
  if (dirpath[0]) {
    if (fat32_find(dirpath, &dir_cluster, 0) != ESUCCESS) {
      return EERROR;
    }
  }

  uint8_t target[11];
  to_83(namebuf, target);

  /* Walk the directory's sectors looking for the entry, then rewrite just
   * that entry's size field and flush the containing sector. */
  uint32_t cluster = dir_cluster;
  while (cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS) {
        return EERROR;
      }
      for (uint32_t off = 0; off + DIR_ENTRY_SIZE <= SECTOR;
           off += DIR_ENTRY_SIZE) {
        struct dir_entry *e = (struct dir_entry *)(sec_buf + off);
        if (e->name[0] == 0x00 || e->name[0] == 0xE5 || e->attr == ATTR_LFN) {
          continue;
        }
        if (memcmp(e->name, target, 11) == 0) {
          e->size = new_size;
          if (blk_write(base + s, sec_buf) != ESUCCESS) {
            return EERROR;
          }
          return ESUCCESS;
        }
      }
    }
    cluster = fat_next(cluster);
  }

  return EERROR;
}

/* Append `count` clusters to the chain ending at `last`, returning the first
 * newly allocated cluster (0 if none was allocated). Each FAT entry is
 * written before the next is allocated, so a failure mid-extension leaves a
 * shorter-but-valid chain rather than a dangling one. */
static uint32_t fat_extend_chain(uint32_t last, uint32_t count) {
  uint32_t first_new = 0;
  uint32_t cur = last;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t nc = fat_alloc_cluster();
    if (!nc) {
      return first_new;
    }
    if (fat_write(cur, nc) != ESUCCESS) {
      return first_new;
    }
    if (!first_new) {
      first_new = nc;
    }
    cur = nc;
  }

  if (first_new) {
    fat_write(cur, FAT32_EOC);
  }
  return first_new;
}

/* Walk to the cluster holding byte `offset`, allocating clusters as needed.
 * On success stores that cluster in *out_cluster and the byte offset within it
 * in *out_inner. `create` decides whether a missing cluster is allocated (a
 * write, which may extend the file) or makes this fail (a read past EOF). */
static int fat_seek_cluster(uint32_t first_cluster, uint32_t offset,
                            uint32_t *out_cluster, uint32_t *out_inner,
                            int create) {
  uint32_t bytes_per_cluster = vol.sectors_per_cluster * SECTOR;
  uint32_t want  = offset / bytes_per_cluster;
  uint32_t cluster = first_cluster;

  for (uint32_t i = 0; i < want; i++) {
    uint32_t next = fat_next(cluster);
    if (next >= FAT32_EOC) {
      if (!create) {
        return EERROR;
      }
      /* fat_extend_chain overwrites last's entry, so no EOC pre-write is
       * needed (and doing it first would be undone immediately). */
      uint32_t grown = fat_extend_chain(cluster, 1);
      if (!grown) {
        return EERROR;
      }
      next = grown;
    }
    cluster = next;
  }

  *out_cluster = cluster;
  *out_inner   = offset % bytes_per_cluster;
  return ESUCCESS;
}

/* Write `len` bytes at byte `offset`, extending the cluster chain as required.
 * On success *out_new_size (when non-NULL) receives offset + len, which the
 * caller stores back into the directory entry. */
int fat32_write_at(uint32_t first_cluster, uint32_t offset, const void *data,
                   uint32_t len, uint32_t *out_new_size) {
  /* Per-call sector buffer — see fat32_read() for why this cannot be a
   * file-scope static. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));

  if (!first_cluster) {
    return EERROR;
  }

  const uint8_t *src = (const uint8_t *)data;
  uint32_t remaining = len;

  while (remaining > 0) {
    uint32_t cluster = 0, inner = 0;
    if (fat_seek_cluster(first_cluster, offset, &cluster, &inner, 1) !=
        ESUCCESS) {
      return EERROR;
    }

    uint32_t base        = cluster_to_sector(cluster);
    uint32_t cluster_end = vol.sectors_per_cluster * SECTOR;

    while (inner < cluster_end && remaining > 0) {
      uint32_t sector_off = inner / SECTOR;
      uint32_t inner_off  = inner % SECTOR;
      uint32_t chunk      = SECTOR - inner_off;
      if (chunk > remaining) {
        chunk = remaining;
      }

      if (inner_off != 0 || chunk != SECTOR) {
        /* Partial sector: read-modify-write so neighbouring bytes survive. */
        if (blk_read(base + sector_off, sec_buf) != ESUCCESS) {
          return EERROR;
        }
        memcpy(sec_buf + inner_off, src, chunk);
      } else {
        /* Whole sector: overwrite directly, skipping the read. */
        memcpy(sec_buf, src, SECTOR);
      }

      if (blk_write(base + sector_off, sec_buf) != ESUCCESS) {
        return EERROR;
      }

      src += chunk;
      remaining -= chunk;
      inner += chunk;
      offset += chunk;
    }
  }

  if (out_new_size) {
    *out_new_size = offset;
  }
  return ESUCCESS;
}

int fat32_read(uint32_t first_cluster, uint32_t size, void *buf,
               uint32_t buf_len) {
  /* Per-call sector buffer. Deliberately NOT a file-scope static: two
   * tasks doing FAT32 I/O concurrently would otherwise read their
   * sectors into the same buffer and silently corrupt each other. A
   * 512-byte local is well within the 16 KiB kernel stack, and no
   * function below holds data in it across a call to another
   * sec_buf user, so shadowing is safe. */
  uint8_t sec_buf[SECTOR] __attribute__((aligned(16)));
  if (size > buf_len) {
    size = buf_len;
  }

  uint8_t *out = (uint8_t *)buf;
  uint32_t remaining = size;
  uint32_t cluster = first_cluster;

  while (remaining > 0 && cluster < FAT32_EOC) {
    uint32_t base = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < vol.sectors_per_cluster && remaining > 0; s++) {
      if (blk_read(base + s, sec_buf) != ESUCCESS) {
        return -1;
      }
      uint32_t chunk = remaining < SECTOR ? remaining : SECTOR;
      memcpy(out, sec_buf, chunk);
      out += chunk;
      remaining -= chunk;
    }
    cluster = fat_next(cluster);
  }
  return (int)(size - remaining);
}

/* ---- Public wrappers for VFS integration ---- */
int fat32_lookup_in_dir(uint32_t dir_cluster, const char *name,
                        uint32_t *out_first_cluster, uint32_t *out_size,
                        int *out_is_dir) {
  uint8_t target[11];
  to_83(name, target);

  uint32_t cluster, size;
  uint8_t attr;

  if (dir_lookup(dir_cluster, target, &cluster, &size, &attr) != ESUCCESS) {
    return EERROR;
  }

  *out_first_cluster = cluster;
  *out_size = size;
  *out_is_dir = (attr & ATTR_DIRECTORY) ? 1 : 0;

  return ESUCCESS;
}

uint32_t fat32_root_cluster(void) { return vol.root_cluster; }
