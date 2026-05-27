#include "fat32.h"
#include "mm/heap/heap.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "vfs/vfs.h"

/* Per-vnode FAT32 state */
typedef struct fat32_priv {
  uint32_t first_cluster; /* cluster where file/dir starts */
  uint32_t size;          /* bytes (0 for directories) */
} fat32_priv_t;

/* Forward decls */
static vnode_t *fat32_lookup(vnode_t *dir, const char *name, size_t namelen);
static int fat32_file_read(vnode_t *n, file_t *f, void *buf, size_t count);

static file_operations_t fat32_file_ops = {
    .read = fat32_file_read, .write = NULL, /* TODO */
};

static vnode_operations_t fat32_dir_ops = {
    .lookup = fat32_lookup,
};

static vnode_t *fat32_lookup(vnode_t *dir, const char *name, size_t namelen) {
  fat32_priv_t *pd = (fat32_priv_t *)dir->private_data;

  if (!pd) {
    return NULL;
  }

  /* Copy to a null-terminated buffer (fat32_lookup_in_dir expects C-string) */
  char tmp[13];

  if (namelen > 12) {
    return NULL;
  }

  for (size_t i = 0; i < namelen; i++) {
    tmp[i] = name[i];
  }

  tmp[namelen] = '\0';

  uint32_t cluster, size;
  int is_dir;

  if (fat32_lookup_in_dir(pd->first_cluster, tmp, &cluster, &size, &is_dir) !=
      ESUCCESS) {
    return NULL;
  }

  // Allocate the per-vnode FAT32 state BEFORE creating the vnode.
  // vfs_create_node prepends the new node onto dir->children immediately,
  // so a kmalloc failure after that point would leave a dangling vnode
  // with NULL private_data permanently stuck in the directory cache.
  fat32_priv_t *cpd = kmalloc(sizeof(fat32_priv_t));
  if (!cpd) {
    return NULL;
  }
  cpd->first_cluster = cluster;
  cpd->size = size;

  vnode_t *child = vfs_create_node(dir, tmp, is_dir ? VNODE_DIR : VNODE_REG);
  if (!child) {
    kfree(cpd);
    return NULL;
  }
  child->private_data = cpd;
  child->size = size;

  if (is_dir) {
    child->v_ops = &fat32_dir_ops;
  } else {
    child->ops = &fat32_file_ops;
  }

  return child;
}

static int fat32_file_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  fat32_priv_t *pd = (fat32_priv_t *)n->private_data;

  if (!pd) {
    return -1;
  }

  if (f->offset >= pd->size) {
    /* EOF */
    return 0;
  }

  uint64_t remaining = pd->size - f->offset;
  uint32_t to_read = (uint32_t)((remaining < count) ? remaining : count);

  /* fat32_read reads from cluster start, so to support arbitrary offsets
   * we read the [0, offset+to_read) prefix into a scratch buffer and copy
   * the requested slice out. O(n) per call rather than O(1), but correct
   * for chunked / non-zero-offset reads. A future fat32_read_at(cluster,
   * offset, len, buf) would be O(1). */
  uint32_t total_needed = (uint32_t)f->offset + to_read;
  uint8_t *tmp = (uint8_t *)kmalloc(total_needed);
  if (!tmp) {
    return -1;
  }

  int got = fat32_read(pd->first_cluster, total_needed, tmp, total_needed);
  if (got < 0) {
    kfree(tmp);
    return -1;
  }
  if ((uint32_t)got <= (uint32_t)f->offset) {
    kfree(tmp);
    return 0; /* short read landed before the requested offset */
  }
  if ((uint32_t)got < total_needed) {
    to_read = (uint32_t)got - (uint32_t)f->offset;
  }

  memcpy(buf, tmp + f->offset, to_read);
  kfree(tmp);

  f->offset += to_read;
  return (int)to_read;
}

/* Mount the FAT32 filesystem root at `path`. `path` must already exist as
 * a directory vnode in the VFS. */
int fat32_vfs_mount(const char *path) {
  vnode_t *mp = vfs_resolve(path);
  if (!mp) {
    uart_printf("[FAT32] Mount point %s does not exist\n", path);
    return EERROR;
  }
  if (mp->type != VNODE_DIR) {
    uart_printf("[FAT32] Mount point %s is not a directory\n", path);
    return EERROR;
  }

  fat32_priv_t *pd = kmalloc(sizeof(fat32_priv_t));
  if (!pd)
    return EERROR;
  pd->first_cluster = fat32_root_cluster();
  pd->size = 0;

  mp->private_data = pd;
  mp->v_ops = &fat32_dir_ops;
  uart_printf("[FAT32] Mounted at %s (root cluster %d)\n", path,
              (uint64_t)pd->first_cluster);
  return ESUCCESS;
}
