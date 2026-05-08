#include "devices.h"
#include "rng/rng.h"
#include "uart/uart.h"
#include "vfs/vfs.h"

/* ---- /dev/uart ---- */
static int console_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  unsigned char *p = buf;

  for (size_t i = 0; i < count; i++) {
    p[i] = uart_getc();
  }

  return (int)count;
}

static int console_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  const char *p = buf;

  for (size_t i = 0; i < count; i++) {
    uart_putc(p[i]);
  }

  return (int)count;
}

static file_operations_t console_ops = {
    .read = console_read,
    .write = console_write,
};

/* ---- /dev/null ---- */
static int null_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  (void)count;

  return 0; /* always EOF */
}

static int null_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;

  return (int)count; /* always accept, discard */
}

static file_operations_t null_ops = {
    .read = null_read,
    .write = null_write,
};

/* ---- /dev/zero ---- */
static int zero_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  unsigned char *p = buf;

  for (size_t i = 0; i < count; i++) {
    p[i] = 0;
  }

  return (int)count;
}

static int zero_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  return (int)count; /* accept and discard */
}

static file_operations_t zero_ops = {
    .read = zero_read,
    .write = zero_write,
};

/* ---- /dev/rng ---- */
static int rng_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;

  return rng_read(buf, (uint32_t)count);
}

static file_operations_t rng_ops = {
    .read = rng_dev_read,
    .write = NULL,
};

/* ---- Entry point ---- */
void devices_register(void) {
  vfs_register_chardev("console", &console_ops);
  vfs_register_chardev("null", &null_ops);
  vfs_register_chardev("zero", &zero_ops);
  vfs_register_chardev("rng", &rng_ops);
}
