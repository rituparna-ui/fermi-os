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
  /* Resize the file to `size` bytes. Optional: NULL means the filesystem does
   * not support truncation, and open(O_TRUNC) is rejected rather than silently
   * ignored. */
  int (*truncate)(struct vnode *node, uint64_t size);
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

/* Create an empty regular file at `path`, for open(O_CREAT).
 *
 * Registered by whichever filesystem is mounted (see fat32_vfs_mount): making a
 * file needs on-disk knowledge — allocating a cluster chain and a directory
 * entry — that the filesystem-agnostic VFS layer deliberately does not have.
 * Returns -1 when no filesystem has registered a handler. */
int vfs_register_create_file(int (*fn)(const char *path));
int vfs_create_file(const char *path);

/* Register a char device under /dev/<name>. Creates /dev if missing. */
vnode_t *vfs_register_chardev(const char *name, file_operations_t *ops);

/* Register a block device under /dev/<name>. Creates /dev if missing. */
vnode_t *vfs_register_blockdev(const char *name, file_operations_t *ops);

/* Seek whence values */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Open file */
/* open(2) flags — identical to Linux/POSIX values so user space can pass
 * <fcntl.h> constants straight through with no translation layer. */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_EXCL   0x0080
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#define O_ACCMODE 0x0003

typedef struct file {
  vnode_t *vnode;
  int64_t  offset;
  /* Access mode from open()'s flags (O_RDONLY / O_WRONLY / O_RDWR). Recorded
   * per descriptor so read/write can reject a call that was never permitted,
   * instead of failing deep inside a driver. */
  int flags;

  /* Number of descriptors in ANY task's table that reference this open file.
   *
   * This is what makes POSIX fork() correct. A fork'd child does NOT get a
   * copy of the open file description: parent and child descriptors must share
   * one, so that a read in the child advances the parent's offset too. Sharing
   * the file_t pointer and refcounting it is what expresses that; copying it
   * would silently give each task an independent offset and break any program
   * that forks with an open file and expects them to stay in step. */
  int refcount;
} file_t;

/* Per-process fd table */
#define MAX_FDS 64

typedef struct fd_table {
  file_t *fds[MAX_FDS];
} fd_table_t;

fd_table_t *fd_table_create(void);
void fd_table_destroy(fd_table_t *t);

/* Duplicate a table for fork(), SHARING the underlying open file descriptions.
 *
 * Every occupied slot points at the same file_t as the source and bumps its
 * refcount, so offsets stay shared between parent and child as POSIX requires.
 * Returns NULL on allocation failure, having changed nothing. */
fd_table_t *fd_table_clone(fd_table_t *src);

/* Drop one reference to an open file, freeing it when the last goes away.
 * Safe to call with NULL. */
void fd_release(file_t *f);

/* Open with explicit flags; returns a new fd or -1.
 *
 * `mode` is accepted and validated for permission bits but not yet applied:
 * this kernel has no ownership model. It is recorded so a later umask() has
 * something to operate on rather than being silently dropped. */
int fd_open_ex(fd_table_t *t, const char *path, int flags, int mode);

/* Convenience wrapper: fd_open_ex(t, path, O_RDONLY, 0). */
int fd_open(fd_table_t *t, const char *path);

int fd_read(fd_table_t *t, int fd, void *buf, size_t count);
int fd_write(fd_table_t *t, int fd, const void *buf, size_t count);
int fd_close(fd_table_t *t, int fd);
int64_t fd_seek(fd_table_t *t, int fd, int64_t offset, int whence);

/* Size of an open file in bytes, or -1. Backs stdio's ftell() and lets callers
 * ask how much is left without a full stat syscall. */
int64_t fd_size(fd_table_t *t, int fd);

/* The vnode behind an open descriptor, or NULL if `fd` is not open.
 *
 * Exposed so metadata queries can report a file's type and size without
 * duplicating the bounds check every accessor here already repeats. The vnode
 * stays owned by the table; callers must not free it. */
vnode_t *fd_vnode(fd_table_t *t, int fd);

#endif
