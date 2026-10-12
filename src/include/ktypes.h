#ifndef KTYPES_H
#define KTYPES_H

/* Kernel ABI: RV64 LP64 (also usable by the LP64 host test compiler).
 * No system headers or runtime are required for these C types.
 */
typedef unsigned char u8_t;
typedef signed char i8_t;
typedef unsigned short u16_t;
typedef signed short i16_t;
typedef unsigned int u32_t;
typedef signed int i32_t;
typedef unsigned long u64_t;
typedef signed long i64_t;
typedef unsigned long ksize_t;
typedef unsigned long uptr_t;

typedef union {
	long double floating;
	long long integer;
	void *pointer;
} kmax_align_t;

_Static_assert(sizeof(u8_t) == 1 && sizeof(u16_t) == 2 &&
	       sizeof(u32_t) == 4 && sizeof(u64_t) == 8 && sizeof(void *) == 8,
	       "kernel requires an LP64 target with 8-bit bytes");
_Static_assert((u8_t)~0u == 255u, "kernel requires 8-bit bytes");

#ifndef NULL
#define NULL ((void *)0)
#endif
#define K_CHAR_BIT 8u
#define U64_C(value) value##UL
#define U32_C(value) value##U
#define U64_MAX 0xffffffffffffffffUL
#define I64_MAX 0x7fffffffffffffffL
#define U32_MAX 0xffffffffU
#define I32_MAX 0x7fffffff
#define KSIZE_MAX U64_MAX
#define UPTR_MAX U64_MAX
/* Compiler layout query only; this does not call or link a library. */
#define K_OFFSETOF(type, member) __builtin_offsetof(type, member)

#endif
