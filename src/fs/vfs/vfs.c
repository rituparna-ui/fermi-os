#include "vfs.h"
#include "mm/heap/heap.h"
#include "strings/strings.h"
#include "uart/uart.h"

#define MAX_VNODES 128

static vnode_t node_pool[MAX_VNODES];
static int node_count = 0;

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
  uart_println("[VFS] Initialized");
}

vnode_t *vfs_root(void) { return root; }

vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type) {
  vnode_t *n = alloc_vnode(name, type);
  if (!n) {
    return NULL;
  }

  n->parent = parent;

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

vnode_t *vfs_resolve(const char *path) {
  if (!path || path[0] != '/') {
    return NULL;
  }

  vnode_t *cur = root;
  path++;

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
      kfree(t->fds[i]);
    }
  }
  kfree(t);
}

static int alloc_fd(fd_table_t *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (!t->fds[i]) {
      return i;
    }
  }

  return -1;
}

int fd_open(fd_table_t *t, const char *path) {
  vnode_t *node = vfs_resolve(path);

  if (!node) {
    return -1;
  }

  int fd = alloc_fd(t);

  if (fd < 0) {
    return -1;
  }

  file_t *f = kmalloc(sizeof(file_t));

  if (!f) {
    return -1;
  }

  f->vnode = node;
  f->offset = 0;
  t->fds[fd] = f;

  return fd;
}

int fd_read(fd_table_t *t, int fd, void *buf, size_t count) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  file_t *f = t->fds[fd];

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

  if (!f->vnode->ops || !f->vnode->ops->write) {
    return -1;
  }

  return f->vnode->ops->write(f->vnode, f, buf, count);
}

int fd_close(fd_table_t *t, int fd) {
  if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
    return -1;
  }

  kfree(t->fds[fd]);
  t->fds[fd] = NULL;

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
