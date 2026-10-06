#ifndef USER_FCNTL_H
#define USER_FCNTL_H

/* <fcntl.h> — open(2) flags. Identical to the kernel's and to Linux's, so
 * these constants cross the syscall boundary untranslated. */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_EXCL   0x0080
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#define O_ACCMODE 0x0003

#endif
