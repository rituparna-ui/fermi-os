#include "devices.h"
#include "blk/blk.h"
#include "rng/rng.h"
#include "console/console.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include "vfs/vfs.h"

#define SECTOR 512

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

/* ---- /dev/blk (raw block device) ----
 * Byte-granular offsets but reads/writes must be sector-aligned:
 *   - f->offset % 512 == 0
 *   - count      % 512 == 0
 * Returns number of bytes transferred, -1 on error.
 */

static int blk_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;

  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }

  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  uint8_t *p = buf;

  for (size_t i = 0; i < sectors; i++) {
    if (blk_read(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }

  f->offset += (int64_t)count;
  return (int)count;
}

static int blk_dev_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }

  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  const uint8_t *p = buf;

  for (size_t i = 0; i < sectors; i++) {
    if (blk_write(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }

  f->offset += (int64_t)count;
  return (int)count;
}

static file_operations_t blk_ops = {
    .read = blk_dev_read,
    .write = blk_dev_write,
};

/* ---- /dev/vcons ---- virtio-console TX side-channel.
 * Anything written here lands on the host file wired up by
 * `-chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt` in QEMU.
 * Reads always return 0 (EOF) — we don't post RX buffers yet. */
static int vcons_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  (void)count;
  return 0;
}

static int vcons_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  int n2 = vcons_send(buf, (uint32_t)count);
  return (n2 < 0) ? -1 : n2;
}

static file_operations_t vcons_ops = {
    .read = vcons_read,
    .write = vcons_write,
};


/* ---- Entry point ---- */
void devices_register(void) {
  vfs_register_chardev("console", &console_ops);
  vfs_register_chardev("null", &null_ops);
  vfs_register_chardev("zero", &zero_ops);
  vfs_register_chardev("rng", &rng_ops);
  vfs_register_chardev("vcons", &vcons_ops);
  vfs_register_blockdev("blk", &blk_ops);
}
