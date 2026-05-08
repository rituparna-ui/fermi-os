#include "vfs.h"
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
