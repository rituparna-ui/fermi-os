#ifndef LIB_PANIC_H
#define LIB_PANIC_H

__attribute__((noreturn)) void kernel_panic(const char *msg);

#endif
