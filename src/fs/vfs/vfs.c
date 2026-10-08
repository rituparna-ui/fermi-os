#include "vfs.h"
#include "mm/heap/heap.h"
#include "strings/strings.h"
#include "uart/uart.h"

#define MAX_VNODES 128

static vnode_t node_pool[MAX_VNODES];
static int node_count = 0;

/* Ops for a directory the VFS materialises in memory (the root, /dev, /mnt).
 * Defined further down, next to the readdir it carries; declared here because
 * vfs_init() and vfs_create_node() assign it before reaching that point. */
static vnode_operations_t vfs_builtin_dir_ops;

static vnode_t *alloc_vnode(const char *name, vnode_type_t type) {
  if (node_count >= MAX_VNODES) {
    return NULL;
  }
  vnode_t *n = &node_pool[node_count++];
  memset(n, 0, sizeof(*n));

  for (int i = 0; i < 63 && name[i]; i++) {
    n->name[i] = name[i];
  }

  n->type = type;
  return n;
}

static vnode_t *root;

void vfs_init(void) {
  root = alloc_vnode("/", VNODE_DIR);
  if (root) {
    root->v_ops = &vfs_builtin_dir_ops;
  }
  uart_println("[VFS] Initialized");
}

vnode_t *vfs_root(void) { return root; }

vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type) {
  vnode_t *n = alloc_vnode(name, type);
  if (!n) {
    return NULL;
  }

  n->parent = parent;

  /* A directory the VFS materialises in memory needs enumeration; a real
   * filesystem installs its own readdir immediately afterwards. */
  if (type == VNODE_DIR) {
    n->v_ops = &vfs_builtin_dir_ops;
  }

  if (parent) {
    n->next = parent->children;
    parent->children = n;
  }

  return n;
}

vnode_t *vfs_register_chardev(const char *name, file_operations_t *ops) {
  vnode_t *dev = vfs_resolve("/dev");

  if (!dev) {
    dev = vfs_create_node(vfs_root(), "dev", VNODE_DIR);
  }

  vnode_t *node = vfs_create_node(dev, name, VNODE_CHR);

  if (node) {
    node->ops = ops;
  }

  uart_printf("[VFS] Registered /dev/%s\n", name);
  return node;
}

vnode_t *vfs_register_blockdev(const char *name, file_operations_t *ops) {
  vnode_t *dev = vfs_resolve("/dev");

  if (!dev) {
    dev = vfs_create_node(vfs_root(), "dev", VNODE_DIR);
  }

  vnode_t *node = vfs_create_node(dev, name, VNODE_BLK);

  if (node) {
    node->ops = ops;
  }

  uart_printf("[VFS] Registered /dev/%s (block)\n", name);
  return node;
}

static int name_match(const char *node_name, const char *s, size_t len) {
  int i;
  for (i = 0; i < (int)len; i++) {
    if (node_name[i] != s[i]) {
      return 0;
    }
  }
  return node_name[i] == '\0';
}

static vnode_t *find_child(vnode_t *dir, const char *name, size_t len) {
  for (vnode_t *c = dir->children; c; c = c->next) {
    if (name_match(c->name, name, len)) {
      return c;
    }
  }
  /* Not cached — query filesystem */
  if (dir->v_ops && dir->v_ops->lookup) {
    return dir->v_ops->lookup(dir, name, len);
  }

  return NULL;
}

vnode_t *vfs_resolve_at(vnode_t *base, const char *path) {
  if (!path) {
    return NULL;
  }

  /* An absolute path starts at the filesystem root and ignores `base`; a
   * relative one starts at `base`.
   *
   * `base` is the filesystem root in every caller today, because this kernel has
   * no per-process working directory and chdir() is not implemented. It is a
   * parameter anyway so adding a real cwd later is a change at the call sites
   * rather than a redesign of the resolver. Concretely: relative paths resolve
   * from the root, so `ls` with no argument (which asks for ".") lists the
   * filesystem root. Documented rather than pretended otherwise. */
  vnode_t *cur;
  if (path[0] == '/') {
    cur = root;
    path++;
  } else {
    cur = base ? base : root;
  }

  while (*path) {
    while (*path == '/') {
      path++;
    }
    if (!*path) {
      break;
    }

    const char *end = path;
    while (*end && *end != '/') {
      end++;
    }
    size_t len = end - path;

    if (cur->type != VNODE_DIR) {
      return NULL;
    }

    if (len == 1 && path[0] == '.') {
      /* stay */
    } else if (len == 2 && path[0] == '.' && path[1] == '.') {
      if (cur->parent) {
        cur = cur->parent;
      }
    } else {
      vnode_t *child = find_child(cur, path, len);
      if (!child) {
        return NULL;
      }
      cur = child;
    }

    path = end;
  }

  return cur;
}

/* Convenience wrapper: resolve relative to the filesystem root. */
vnode_t *vfs_resolve(const char *path) {
  return vfs_resolve_at(root, path);
}

fd_table_t *fd_table_create(void) {
  fd_table_t *t = kmalloc(sizeof(fd_table_t));

  if (t) {
    memset(t, 0, sizeof(*t));
  }

  return t;
}

void fd_table_destroy(fd_table_t *t) {
  if (!t) {
    return;
  }

  for (int i = 0; i < MAX_FDS; i++) {
    if (t->fds[i]) {
      /* Release, don't free: a fork'd sibling may still be using the same
       * open file description. */
      fd_release(t->fds[i]);
      t->fds[i] = NULL;
    }
  }
  kfree(t);
}

/* Hook installed by the mounted filesystem so open(O_CREAT) can create real
 * on-disk files. NULL until something registers one, which is why
 * vfs_create_file() fails cleanly when no filesystem is present. */
static int (*vfs_create_file_hook)(const char *path);

int vfs_register_create_file(int (*fn)(const char *path)) {
  vfs_create_file_hook = fn;
  return 0;
}

int vfs_create_file(const char *path) {
  if (!vfs_create_file_hook) {
    return -1;
  }
  return vfs_create_file_hook(path);
}

static int alloc_fd(fd_table_t *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (!t->fds[i]) {
      return i;
    }
  }

  return -1;
}

void fd_release(file_t *f) {
  if (!f) {
    return;
  }
  if (--f->refcount > 0) {
    return; /* another task still holds it (e.g. a fork'd child) */
  }
  kfree(f);
}

fd_table_t *fd_table_clone(fd_table_t *src) {
  if (!src) {
    return NULL;
  }
  fd_table_t *dst = fd_table_create();
  if (!dst) {
    return NULL;
  }
  for (int i = 0; i < MAX_FDS; i++) {
    if (!src->fds[i]) {
      continue;
    }
    /* Share the open file description, not a copy of it: that is what makes a
     * child observe the parent's file offset. */
    dst->fds[i] = src->fds[i];
    src->fds[i]->refcount++;
  }
  return dst;
}

int fd_open_ex(fd_table_t *t, const char *path, int flags, int mode) {
  /* Reject a nonsense access mode up front so a caller passing a garbage flags
   * word gets one clean failure instead of a confusing one later. */
  if ((flags & O_ACCMODE) > O_RDWR) {
    return -1;
  }
  /* O_EXCL is meaningless without O_CREAT; honouring it anyway would mask a
   * caller bug. */
  if ((flags & O_EXCL) && !(flags & O_CREAT)) {
    return -1;
  }

  vnode_t *node = vfs_resolve(path);

  if (!node) {
    if (!(flags & O_CREAT)) {
      return -1;
    }
    /* Create an empty file. The VFS layer is filesystem-agnostic, so this goes
     * through whichever hook the mounted filesystem installed. */
    if (vfs_create_file(path) < 0) {
      return -1;
    }
    node = vfs_resolve(path);
    if (!node) {
      return -1;
    }
  } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
    return -1; /* O_EXCL|O_CREAT must fail when the file exists */
  }

  /* O_TRUNC clears the contents. Done after resolution so a file that does not
   * exist yet (O_CREAT) is naturally already empty. */
  if ((flags & O_TRUNC) && node->ops && node->ops->truncate) {
    if (node->ops->truncate(node, 0) < 0) {
      return -1;
    }
    node->size = 0;
  }

  int fd = alloc_fd(t);
  if (fd < 0) {
    return -1;
  }

  file_t *f = kmalloc(sizeof(file_t));
  if (!f) {
    return -1;
  }

  f->vnode    = node;
  f->flags    = flags & O_ACCMODE;
  f->refcount = 1;
  /* O_APPEND means every write goes to the end. That is a property of the
   * write path, so record the flag and start the descriptor at the end. */
  f->offset = (flags & O_APPEND) ? node->size : 0;

  (void)mode; /* no ownership model yet: accepted, not enforced */
  t->fds[fd] = f;

  return fd;
}

int fd_open(fd_table_t *t, const char *path) {
  return fd_open_ex(t, path, O_RDONLY, 0);
}

int fd_read(fd_table_t *t, int fd, void *buf, size_t count) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  file_t *f = t->fds[fd];

  /* A write-only descriptor must not read and vice versa. Enforced here rather
   * than per-driver so every filesystem reports the same failure instead of
   * two different ones. */
  if (f->flags == O_WRONLY) {
    return -1;
  }

  if (!f->vnode->ops || !f->vnode->ops->read) {
    return -1;
  }

  return f->vnode->ops->read(f->vnode, f, buf, count);
}

int fd_write(fd_table_t *t, int fd, const void *buf, size_t count) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  file_t *f = t->fds[fd];

  if (f->flags == O_RDONLY) {
    return -1;
  }

  if (!f->vnode->ops || !f->vnode->ops->write) {
    return -1;
  }

  return f->vnode->ops->write(f->vnode, f, buf, count);
}

/* Enumerate a directory the VFS holds in memory -- the root, /dev, /mnt and
 * friends -- by walking its `children` list.
 *
 * Real filesystems (FAT32) supply their own readdir that reads on-disk entries;
 * this covers the synthetic directories, which have no on-disk existence at all.
 * Without it, `ls` with no argument fails: "." resolves to the VFS root, and the
 * root is exactly one of these.
 *
 * The cookie is an index into the child list, so resuming re-walks from the
 * head. The lists here are a handful of entries long, so that is cheaper than
 * the bookkeeping a persistent cursor would need. */
static int vfs_builtin_readdir(vnode_t *dir, uint64_t *offset,
                               vfs_dirent_t *out, int max, int *eof) {
  if (eof) {
    *eof = 0;
  }
  if (!dir || dir->type != VNODE_DIR) {
    return -1;
  }

  /* `seen` counts children examined. `*offset` says how many were already
   * handed out, and becomes the number examined here.
   *
   * These are different quantities on purpose: a child that is examined but
   * cannot fit in a full buffer must NOT be counted, or the next call would skip
   * it and the entry would vanish. */
  uint64_t seen = 0;
  int      n    = 0;
  int      exhausted = 1; /* cleared if we stop early with entries left */

  for (vnode_t *c = dir->children; c; c = c->next) {
    if (seen < *offset) {
      seen++;
      continue; /* already returned to the caller */
    }
    if (n == max) {
      exhausted = 0; /* buffer full; `c` is unconsumed */
      break;
    }
    seen++;

    size_t len = 0;
    while (len < sizeof(out[n].name) - 1 && c->name[len] != '\0') {
      out[n].name[len] = c->name[len];
      len++;
    }
    out[n].name[len] = '\0';
    out[n].size      = c->size;
    out[n].is_dir    = (c->type == VNODE_DIR);
    /* Synthetic nodes have no filesystem identity, so their inode is just their
     * address. Stable for the node's lifetime, which is all a caller can rely
     * on; documented as carrying no other meaning. */
    out[n].id        = (uint64_t)(uintptr_t)c;
    n++;
  }

  /* Ran off the end of the list, rather than stopping because the buffer filled. */
  if (exhausted && eof) {
    *eof = 1;
  }
  *offset = seen;
  return n;
}

/* Ops for a directory the VFS itself created. FAT32 children overwrite
 * v_ops with fat32_dir_ops once it knows they are on-disk. */
static vnode_operations_t vfs_builtin_dir_ops = {
    .lookup  = NULL,
    .readdir = vfs_builtin_readdir,
};

int vfs_readdir(vnode_t *dir, uint64_t *offset, vfs_dirent_t *out, int max,
                int *eof) {
  if (eof) {
    *eof = 0;
  }
  if (!dir || dir->type != VNODE_DIR) {
    return -1; /* ENOTDIR */
  }
  if (!dir->v_ops || !dir->v_ops->readdir) {
    return -1; /* this filesystem cannot enumerate */
  }
  return dir->v_ops->readdir(dir, offset, out, max, eof);
}

vnode_t *fd_vnode(fd_table_t *t, int fd) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return NULL;
  }
  return t->fds[fd]->vnode;
}

int64_t fd_size(fd_table_t *t, int fd) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }
  return (int64_t)t->fds[fd]->vnode->size;
}

int fd_close(fd_table_t *t, int fd) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  /* Detach from this table first, then release. If a fork'd sibling shares the
   * description it keeps it alive; only the last close frees. */
  file_t *f = t->fds[fd];
  t->fds[fd] = NULL;
  fd_release(f);

  return 0;
}

int64_t fd_seek(fd_table_t *t, int fd, int64_t offset, int whence) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  file_t *f = t->fds[fd];

  /* char devices are not seekable */
  if (f->vnode->type == VNODE_CHR) {
    return -1;
  }

  int64_t new_off;
  switch (whence) {
  case SEEK_SET:
    new_off = offset;
    break;
  case SEEK_CUR:
    new_off = f->offset + offset;
    break;
  case SEEK_END:
    /* For regular files vnode->size is populated at lookup time (FAT32
     * stores it in the directory entry). Char/block devices fall through
     * to the failure path below. */
    if (f->vnode->type != VNODE_REG) {
      return -1;
    }
    new_off = (int64_t)f->vnode->size + offset;
    break;
  default:
    return -1;
  }

  if (new_off < 0) {
    return -1;
  }

  f->offset = new_off;
  return new_off;
}
