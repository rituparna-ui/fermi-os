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
static int fat32_file_write(vnode_t *n, file_t *f, const void *buf,
                            size_t count);
static int fat32_file_truncate(vnode_t *n, uint64_t size);
static int fat32_rel_path(const char *vpath, char *out, size_t outlen);

static file_operations_t fat32_file_ops = {
    .read = fat32_file_read,
    .write = fat32_file_write,
    .truncate = fat32_file_truncate,
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

/* Reconstruct a vnode's on-disk path by walking parent links.
 *
 * Writes and truncates have to update the directory entry, and FAT32 identifies
 * files by path, not by cluster — so a write op needs the path of the vnode it
 * is being called on. Rebuilding it here keeps the caller from having to thread
 * the original open() path down through file_operations_t for every op.
 *
 * Returns 0 on success, -1 if the chain is longer than `buflen` or is broken. */
static int fat32_build_vnode_path(vnode_t *n, char *buf, size_t buflen) {
  /* Collect vnodes root-first, then emit them leaf-to-root. The tree depth is
   * tiny (FAT32 paths), so a fixed stack is enough; an over-deep chain is
   * rejected rather than walked off the end. */
  vnode_t *chain[16];
  int      depth = 0;

  for (vnode_t *p = n; p && p->parent; p = p->parent) {
    if (depth >= (int)(sizeof(chain) / sizeof(chain[0]))) {
      return -1;
    }
    chain[depth++] = p;
  }

  size_t pos = 0;
  for (int i = depth - 1; i >= 0; i--) {
    const char *name = chain[i]->name;
    size_t      len = strlen(name);
    if (pos + len + 2 > buflen) {
      return -1;
    }
    buf[pos++] = '/';
    for (size_t k = 0; k < len; k++) {
      buf[pos++] = name[k];
    }
  }
  if (pos + 1 > buflen) {
    return -1;
  }
  buf[pos] = '\0';
  return 0;
}

/* Write at the descriptor's offset, extending the file.
 *
 * fat32_write_at() does the cluster-chain work; the fiddly part here is that a
 * FAT32 file's length lives in the directory entry, so the in-memory size must
 * be pushed back to disk after every write. A size that is correct in RAM but
 * stale on disk produces a file that looks truncated to the next reader —
 * including this kernel after a reopen. */
static int fat32_file_write(vnode_t *n, file_t *f, const void *buf,
                            size_t count) {
  fat32_priv_t *pd = (fat32_priv_t *)n->private_data;
  if (!pd || count == 0) {
    return count == 0 ? 0 : -1;
  }

  uint32_t new_size = 0;
  if (fat32_write_at(pd->first_cluster, (uint32_t)f->offset, buf,
                      (uint32_t)count, &new_size) != ESUCCESS) {
    return -1;
  }

  /* Persist the new length. The path is needed, so rebuild it from the vnode
   * chain rather than duplicating whatever the caller passed in. */
  char path[256];
  if (fat32_build_vnode_path(n, path, sizeof(path)) != 0) {
    return -1;
  }
  char rel[128];
  if (fat32_rel_path(path, rel, sizeof(rel)) < 0) {
    return -1;
  }
  if (fat32_truncate_path(rel, new_size) != ESUCCESS) {
    return -1;
  }

  pd->size = new_size;
  n->size  = new_size;
  f->offset += (int64_t)count;
  return (int)count;
}

/* O_TRUNC / ftruncate support.
 *
 * Only shrinking is implemented: a zero-length file needs no cluster work at
 * all, and it is the case stdio's "wb" mode actually hits. Growing would need a
 * zero-fill path and is refused rather than half-done. */
static int fat32_file_truncate(vnode_t *n, uint64_t size) {
  fat32_priv_t *pd = (fat32_priv_t *)n->private_data;
  if (!pd) {
    return -1;
  }
  if (size > (uint64_t)pd->size) {
    return -1; /* growing is not implemented; see the note above */
  }

  char path[256];
  if (fat32_build_vnode_path(n, path, sizeof(path)) != 0) {
    return -1;
  }
  char rel[128];
  if (fat32_rel_path(path, rel, sizeof(rel)) < 0) {
    return -1;
  }
  if (fat32_truncate_path(rel, (uint32_t)size) != ESUCCESS) {
    return -1;
  }

  pd->size = (uint32_t)size;
  n->size  = size;
  return 0;
}

/* Where this filesystem is mounted in the VFS tree, e.g. "/mnt/fat32".
 * Remembered at mount time so paths can be translated between the two views. */
static char fat32_mount_path[64] = "";

/* Translate a VFS path into one relative to the FAT32 volume root.
 *
 * The VFS calls us with "/mnt/fat32/FILE.TXT", but the FAT32 layer addresses
 * files from the volume root and has no concept of the mount point. Passing the
 * VFS path straight through made every create and truncate fail with ENOENT:
 * FAT32 was looking for a subdirectory literally named "mnt".
 *
 * Returns 0 on success, -1 if `vpath` is not under this mount point. */
static int fat32_rel_path(const char *vpath, char *out, size_t outlen) {
  size_t mlen = strlen(fat32_mount_path);
  if (mlen == 0) {
    return -1;
  }
  if (strncmp(vpath, fat32_mount_path, mlen) != 0) {
    return -1; /* not ours */
  }
  const char *rest = vpath + mlen;
  while (*rest == '/') {
    rest++;
  }
  size_t rlen = strlen(rest);
  if (rlen + 1 > outlen) {
    return -1;
  }
  memcpy(out, rest, rlen + 1);
  return 0;
}

/* open(O_CREAT) hook: make an empty file, then let vfs_resolve() build the
 * vnode for it. Registered at mount time. */
static int fat32_create_file(const char *path) {
  char rel[128];
  if (fat32_rel_path(path, rel, sizeof(rel)) < 0) {
    return -1;
  }
  return fat32_create(rel, 0, 0) == ESUCCESS ? 0 : -1;
}

/* Mount the FAT32 filesystem root at `path`. `path` must already exist as
 * a directory vnode in the VFS. */
int fat32_vfs_mount(const char *path) {
  /* Remember where we live so VFS paths can be translated to volume-relative
   * ones. fat32_create_file() and the truncate/write paths depend on it. */
  size_t plen = strlen(path);
  if (plen < sizeof(fat32_mount_path)) {
    memcpy(fat32_mount_path, path, plen + 1);
    /* Normalise: no trailing slash, so path + mlen is an exact prefix. */
    while (plen > 1 && fat32_mount_path[plen - 1] == '/') {
      fat32_mount_path[--plen] = '\0';
    }
  }

  /* Teach the VFS how to create files on this filesystem. */
  vfs_register_create_file(fat32_create_file);

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
