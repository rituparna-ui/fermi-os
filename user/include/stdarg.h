#ifndef USER_STDARG_H
#define USER_STDARG_H

/*
 * <stdarg.h> for the aarch64 AAPCS64 ABI.
 *
 * This forwards to GCC's own builtins rather than re-declaring va_list's
 * layout. On AAPCS64 va_list IS `struct __va_list_tag[1]` — an array, not a
 * struct — and GCC treats __builtin_va_start as taking it by reference.
 * Declaring our own struct under the same name conflicts with the compiler's
 * built-in definition and produces "cannot pass rvalue to reference parameter"
 * at every call site.
 *
 * The builtins are available in freestanding builds (-ffreestanding), so there
 * is nothing to implement here.
 */

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

#endif
