#ifndef FS_VFS_H
#define FS_VFS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  VNODE_REG, // Regular file
  VNODE_DIR, // Directory
  VNODE_CHR, // Char dev
} vnode_type_t;

struct vnode;
struct file;

typedef struct file_operations {
  int (*read)(struct vnode *node, struct file *f, void *buf, size_t count);
  int (*write)(struct vnode *node, struct file *f, const void *buf,
               size_t count);
} file_operations_t;

typedef struct vnode {
  char name[64];
  vnode_type_t type;
  file_operations_t *ops;
  struct vnode *parent;
  struct vnode *children;
  struct vnode *next;
} vnode_t;

void vfs_init(void);
vnode_t *vfs_root(void);
vnode_t *vfs_create_node(vnode_t *parent, const char *name, vnode_type_t type);
vnode_t *vfs_resolve(const char *path);

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

#endif
