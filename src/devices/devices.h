#ifndef DEVICES_H
#define DEVICES_H

/* Register built-in/virtual devices under /dev/:
 *   /dev/console — UART (stdin/stdout/stderr)
 *   /dev/null    — discards writes, reads return 0 (EOF)
 *   /dev/zero    — reads return zeros, writes discarded
 *   /dev/rng     — random bytes from virtio-rng
 *   /dev/blk0     — raw virtio-blk device (sector-aligned reads/writes)
 */
void devices_register(void);

#endif
