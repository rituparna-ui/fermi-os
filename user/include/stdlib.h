#ifndef USER_STDLIB_H
#define USER_STDLIB_H

#include <stddef.h>

/*
 * <stdlib.h> — general utilities. Allocator lives in user/lib/malloc.c and is
 * backed by the kernel's brk() arena.
 */

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t n);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

/* Usable payload bytes for a live allocation, or 0 if p isn't one. Mainly for
 * tests and allocator debugging. */
size_t malloc_usable_size(void *p);

/* Total bytes the allocator has requested from the kernel, including headers. */
size_t malloc_heap_bytes(void);

/* Number of live (allocated) blocks. */
size_t malloc_live_blocks(void);

int abs(int n);
long labs(long n);

/* qsort over an array of `count` elements of `size` bytes, comparing through
 * cmp like the standard: negative if a<b, 0 if equal, positive if a>b.
 * Not stable. */
void qsort(void *base, size_t count, size_t size,
           int (*cmp)(const void *, const void *));

void abort(void);
void exit(int status);

#endif