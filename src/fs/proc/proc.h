#ifndef FS_PROC_H
#define FS_PROC_H

/* /proc-style virtual filesystem. Each file synthesises its content
 * on every read() out of live kernel state \u2014 nothing is persisted.
 *
 * Files exposed:
 *   /proc/uptime    "<seconds>\n"
 *   /proc/meminfo   PMM + heap stats (one key per line)
 *   /proc/tasks     PID / state / name table
 *   /proc/version   kernel banner / build info
 *   /proc/balloon   virtio-balloon actual / host-target page counts
 */
void proc_init(void);

#endif
