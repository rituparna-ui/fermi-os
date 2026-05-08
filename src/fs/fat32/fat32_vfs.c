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

  vnode_t *child = vfs_create_node(dir, tmp, is_dir ? VNODE_DIR : VNODE_REG);
  if (!child) {
    return NULL;
  }

  fat32_priv_t *cpd = kmalloc(sizeof(fat32_priv_t));

  if (!cpd) {
    return NULL;
  }

  cpd->first_cluster = cluster;
  cpd->size = size;
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

  /* read from offset 0 only. Real fat32_read doesn't take
   * an offset, so for now we require f->offset == 0. */
  if (f->offset != 0) {
    return -1;
  }

  uint32_t to_read = pd->size < count ? pd->size : (uint32_t)count;
  int n_read = fat32_read(pd->first_cluster, to_read, buf, to_read);

  if (n_read < 0) {
    return -1;
  }

  f->offset += n_read;
  return n_read;
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
