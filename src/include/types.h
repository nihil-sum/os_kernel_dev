#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

typedef uint64_t u64_t;
typedef uint32_t u32_t;
typedef uint16_t u16_t;
typedef uint8_t u8_t;
typedef int64_t i64_t;
typedef int32_t i32_t;
typedef int16_t i16_t;
typedef int8_t i8_t;

/* Compatibility: existing callers that include types.h still see callable.
 * New callback users should include callable.h directly.
 */
#include "callable.h"

#endif
