#include "kstring.h"

/* GCC may emit these calls for aggregate initialization/copy even in a
 * freestanding build. Compile with -fno-builtin to keep these loops local.
 */
void *memset(void *dst, int value, ksize_t size) {
        unsigned char *p = dst;
        for (ksize_t i = 0; i < size; i++) p[i] = (unsigned char)value;
        return dst;
}

void *memcpy(void *dst, const void *src, ksize_t size) {
        unsigned char *d = dst;
        const unsigned char *s = src;
        for (ksize_t i = 0; i < size; i++) d[i] = s[i];
        return dst;
}
