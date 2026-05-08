#ifndef FS_VFS_H
#define FS_VFS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  VNODE_REG, // Regular file
  VNODE_DIR, // Directory
  VNODE_CHR, // Char dev
  VNODE_BLK, // Block dev
} vnode_type_t;

struct vnode;
struct file;

/*
 * file_operations is the VFS's per-vnode "vtable": a set of function
 * pointers that each filesystem/device fills in with its own read/write
 * implementation. When the kernel handles read(fd, ...), it looks up the
 * vnode for that fd and calls vnode->ops->read(...) — the actual function
 * invoked is decided at runtime based on which vnode it is.
 *
 * This is C's way of doing dynamic polymorphism:
 *   /dev/console → ops = UART read/write
 *   /dev/rng     → ops = virtio-rng read
 *   /dev/zero    → ops = fill-with-zeros
 *   future fat32 regular file → ops = fat32 read/write
 *
 * Equivalent C++ would be: an abstract class `Vnode` with virtual read/
 * write methods, and subclasses for each driver/filesystem. The compiler
 * would generate a vtable behind the scenes — which is exactly what we
 * build by hand here.
 */
typedef struct file_operations {
  int (*read)(struct vnode *node, struct file *f, void *buf, size_t count);
  int (*write)(struct vnode *node, struct file *f, const void *buf,
               size_t count);
} file_operations_t;

/*
 * vnode_operations: per-directory vtable for tree traversal.
 * Filesystems fill in `lookup` to resolve a name inside a directory on demand.
 */
typedef struct vnode_operations {
  struct vnode *(*lookup)(struct vnode *dir, const char *name, size_t namelen);
} vnode_operations_t;

typedef struct vnode {
  char name[64];
  vnode_type_t type;
  file_operations_t *ops;    /* how to read/write this node */
  vnode_operations_t *v_ops; /* how to traverse (if directory) */
  void *private_data;        /* fs/driver-specific per-vnode state */
  uint64_t size;             /* for regular files */
  struct vnode *parent;
  struct vnode *children;
  struct vnode *next;
} vnode_t;

void vfs_init(void);
vnode_t *vfs_root(void);
vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type);
vnode_t *vfs_resolve(const char *path);

/* Register a char device under /dev/<name>. Creates /dev if missing. */
vnode_t *vfs_register_chardev(const char *name, file_operations_t *ops);

/* Register a block device under /dev/<name>. Creates /dev if missing. */
vnode_t *vfs_register_blockdev(const char *name, file_operations_t *ops);

/* Seek whence values */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Open file */
typedef struct file {
  vnode_t *vnode;
  int64_t offset;
} file_t;

/* Per-process fd table */
#define MAX_FDS 64

typedef struct fd_table {
  file_t *fds[MAX_FDS];
} fd_table_t;

fd_table_t *fd_table_create(void);
void fd_table_destroy(fd_table_t *t);
int fd_open(fd_table_t *t, const char *path);
int fd_read(fd_table_t *t, int fd, void *buf, size_t count);
int fd_write(fd_table_t *t, int fd, const void *buf, size_t count);
int fd_close(fd_table_t *t, int fd);
int64_t fd_seek(fd_table_t *t, int fd, int64_t offset, int whence);

#endif
