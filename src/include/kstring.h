#ifndef KSTRING_H
#define KSTRING_H

#include "ktypes.h"

/* Implemented by this kernel in src/types/string.c. The conventional symbol
 * names also satisfy copies/initializations emitted by the compiler.
 */
void *memset(void *dst, int value, ksize_t size);
void *memcpy(void *dst, const void *src, ksize_t size);

#endif
