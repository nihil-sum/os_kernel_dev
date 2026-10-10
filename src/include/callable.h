#ifndef CALLABLE_H
#define CALLABLE_H

#include <stdint.h>
#include "status.h"

/* V means no arguments; P means a borrowed void * context.
 * Legacy FN_V_V/fvv retains its original context-taking signature.
 */
typedef enum {
	fvv = 1,
	fu64v,
	fvv0,
	fu8v,
	fu8p,
	fu16v,
	fu16p,
	fu32v,
	fu32p,
	fu64p,
	fi8v,
	fi8p,
	fi16v,
	fi16p,
	fi32v,
	fi32p,
	fi64v,
	fi64p,
	fptrv,
	fptrp,
} CallableType;

typedef void (*FN_V_V)(void *);
typedef uint64_t (*FN_U64_V)(void);
typedef void (*FN_V_V0)(void);
typedef uint8_t (*FN_U8_V)(void);
typedef uint8_t (*FN_U8_P)(void *);
typedef uint16_t (*FN_U16_V)(void);
typedef uint16_t (*FN_U16_P)(void *);
typedef uint32_t (*FN_U32_V)(void);
typedef uint32_t (*FN_U32_P)(void *);
typedef uint64_t (*FN_U64_P)(void *);
typedef int8_t (*FN_I8_V)(void);
typedef int8_t (*FN_I8_P)(void *);
typedef int16_t (*FN_I16_V)(void);
typedef int16_t (*FN_I16_P)(void *);
typedef int32_t (*FN_I32_V)(void);
typedef int32_t (*FN_I32_P)(void *);
typedef int64_t (*FN_I64_V)(void);
typedef int64_t (*FN_I64_P)(void *);
typedef void *(*FN_PTR_V)(void);
typedef void *(*FN_PTR_P)(void *);

struct callable {
	CallableType tag;
	union {
		FN_V_V fnv;
		FN_U64_V fnu64v;
		FN_V_V0 fnvv0;
		FN_U8_V fnu8v;
		FN_U8_P fnu8p;
		FN_U16_V fnu16v;
		FN_U16_P fnu16p;
		FN_U32_V fnu32v;
		FN_U32_P fnu32p;
		FN_U64_P fnu64p;
		FN_I8_V fni8v;
		FN_I8_P fni8p;
		FN_I16_V fni16v;
		FN_I16_P fni16p;
		FN_I32_V fni32v;
		FN_I32_P fni32p;
		FN_I64_V fni64v;
		FN_I64_P fni64p;
		FN_PTR_V fnptrv;
		FN_PTR_P fnptrp;
	};
	/* Ignored by callbacks with no arguments. */
	void *arg;
};

static inline struct callable callable_from_fnv(FN_V_V fn, void *arg) {
	return (struct callable){ .tag = fvv, .fnv = fn, .arg = arg };
}

static inline struct callable callable_from_fnu64v(FN_U64_V fn) {
	return (struct callable){ .tag = fu64v, .fnu64v = fn, .arg = 0 };
}

static inline struct callable callable_from_fnvv0(FN_V_V0 fn) {
	return (struct callable){ .tag = fvv0, .fnvv0 = fn, .arg = 0 };
}

static inline struct callable callable_from_fnu8v(FN_U8_V fn) {
	return (struct callable){ .tag = fu8v, .fnu8v = fn, .arg = 0 };
}

static inline struct callable callable_from_fnu8p(FN_U8_P fn, void *arg) {
	return (struct callable){ .tag = fu8p, .fnu8p = fn, .arg = arg };
}

static inline struct callable callable_from_fnu16v(FN_U16_V fn) {
	return (struct callable){ .tag = fu16v, .fnu16v = fn, .arg = 0 };
}

static inline struct callable callable_from_fnu16p(FN_U16_P fn, void *arg) {
	return (struct callable){ .tag = fu16p, .fnu16p = fn, .arg = arg };
}

static inline struct callable callable_from_fnu32v(FN_U32_V fn) {
	return (struct callable){ .tag = fu32v, .fnu32v = fn, .arg = 0 };
}

static inline struct callable callable_from_fnu32p(FN_U32_P fn, void *arg) {
	return (struct callable){ .tag = fu32p, .fnu32p = fn, .arg = arg };
}

static inline struct callable callable_from_fnu64p(FN_U64_P fn, void *arg) {
	return (struct callable){ .tag = fu64p, .fnu64p = fn, .arg = arg };
}

static inline struct callable callable_from_fni8v(FN_I8_V fn) {
	return (struct callable){ .tag = fi8v, .fni8v = fn, .arg = 0 };
}

static inline struct callable callable_from_fni8p(FN_I8_P fn, void *arg) {
	return (struct callable){ .tag = fi8p, .fni8p = fn, .arg = arg };
}

static inline struct callable callable_from_fni16v(FN_I16_V fn) {
	return (struct callable){ .tag = fi16v, .fni16v = fn, .arg = 0 };
}

static inline struct callable callable_from_fni16p(FN_I16_P fn, void *arg) {
	return (struct callable){ .tag = fi16p, .fni16p = fn, .arg = arg };
}

static inline struct callable callable_from_fni32v(FN_I32_V fn) {
	return (struct callable){ .tag = fi32v, .fni32v = fn, .arg = 0 };
}

static inline struct callable callable_from_fni32p(FN_I32_P fn, void *arg) {
	return (struct callable){ .tag = fi32p, .fni32p = fn, .arg = arg };
}

static inline struct callable callable_from_fni64v(FN_I64_V fn) {
	return (struct callable){ .tag = fi64v, .fni64v = fn, .arg = 0 };
}

static inline struct callable callable_from_fni64p(FN_I64_P fn, void *arg) {
	return (struct callable){ .tag = fi64p, .fni64p = fn, .arg = arg };
}

static inline struct callable callable_from_fnptrv(FN_PTR_V fn) {
	return (struct callable){ .tag = fptrv, .fnptrv = fn, .arg = 0 };
}

static inline struct callable callable_from_fnptrp(FN_PTR_P fn, void *arg) {
	return (struct callable){ .tag = fptrp, .fnptrp = fn, .arg = arg };
}

/* Invoke once. For a non-void return, result must point to a writable
 * object of exactly the callback's return type (e.g. int16_t or void *).
 * Void callbacks leave result untouched and accept a NULL result.
 * NULL callables/functions, unknown tags, and missing non-void results fail
 * without invocation or changing result. A zero-initialized callable is invalid.
 * The caller owns the context and must keep it alive during invocation.
 * Keep tag consistent with the active function member; prefer constructors.
 */
static inline enum status callable_call(const struct callable *callable,
				       void *result) {
	if (!callable)
		return FAILED;

	switch (callable->tag) {
	case fvv:
		if (!callable->fnv)
			return FAILED;
		callable->fnv(callable->arg);
		return SUCCESS;
	case fu64v:
		if (!callable->fnu64v || !result)
			return FAILED;
		*(uint64_t *)result = callable->fnu64v();
		return SUCCESS;
	case fvv0:
		if (!callable->fnvv0)
			return FAILED;
		callable->fnvv0();
		return SUCCESS;
	case fu8v:
		if (!callable->fnu8v || !result)
			return FAILED;
		*(uint8_t *)result = callable->fnu8v();
		return SUCCESS;
	case fu8p:
		if (!callable->fnu8p || !result)
			return FAILED;
		*(uint8_t *)result = callable->fnu8p(callable->arg);
		return SUCCESS;
	case fu16v:
		if (!callable->fnu16v || !result)
			return FAILED;
		*(uint16_t *)result = callable->fnu16v();
		return SUCCESS;
	case fu16p:
		if (!callable->fnu16p || !result)
			return FAILED;
		*(uint16_t *)result = callable->fnu16p(callable->arg);
		return SUCCESS;
	case fu32v:
		if (!callable->fnu32v || !result)
			return FAILED;
		*(uint32_t *)result = callable->fnu32v();
		return SUCCESS;
	case fu32p:
		if (!callable->fnu32p || !result)
			return FAILED;
		*(uint32_t *)result = callable->fnu32p(callable->arg);
		return SUCCESS;
	case fu64p:
		if (!callable->fnu64p || !result)
			return FAILED;
		*(uint64_t *)result = callable->fnu64p(callable->arg);
		return SUCCESS;
	case fi8v:
		if (!callable->fni8v || !result)
			return FAILED;
		*(int8_t *)result = callable->fni8v();
		return SUCCESS;
	case fi8p:
		if (!callable->fni8p || !result)
			return FAILED;
		*(int8_t *)result = callable->fni8p(callable->arg);
		return SUCCESS;
	case fi16v:
		if (!callable->fni16v || !result)
			return FAILED;
		*(int16_t *)result = callable->fni16v();
		return SUCCESS;
	case fi16p:
		if (!callable->fni16p || !result)
			return FAILED;
		*(int16_t *)result = callable->fni16p(callable->arg);
		return SUCCESS;
	case fi32v:
		if (!callable->fni32v || !result)
			return FAILED;
		*(int32_t *)result = callable->fni32v();
		return SUCCESS;
	case fi32p:
		if (!callable->fni32p || !result)
			return FAILED;
		*(int32_t *)result = callable->fni32p(callable->arg);
		return SUCCESS;
	case fi64v:
		if (!callable->fni64v || !result)
			return FAILED;
		*(int64_t *)result = callable->fni64v();
		return SUCCESS;
	case fi64p:
		if (!callable->fni64p || !result)
			return FAILED;
		*(int64_t *)result = callable->fni64p(callable->arg);
		return SUCCESS;
	case fptrv:
		if (!callable->fnptrv || !result)
			return FAILED;
		*(void **)result = callable->fnptrv();
		return SUCCESS;
	case fptrp:
		if (!callable->fnptrp || !result)
			return FAILED;
		*(void **)result = callable->fnptrp(callable->arg);
		return SUCCESS;
	default:
		return FAILED;
	}
}

/* Validate without invoking. Context pointers remain borrowed and may be NULL.
 * Constructors keep the tag consistent with the active function member.
 */
static inline int callable_valid(const struct callable *callable) {
	if (!callable) return 0;
#define CALLABLE_VALID(tag_value, member) \
	case tag_value: return callable->member != 0
	switch (callable->tag) {
	CALLABLE_VALID(fvv, fnv);
	CALLABLE_VALID(fvv0, fnvv0);
	CALLABLE_VALID(fu8v, fnu8v);
	CALLABLE_VALID(fu8p, fnu8p);
	CALLABLE_VALID(fu16v, fnu16v);
	CALLABLE_VALID(fu16p, fnu16p);
	CALLABLE_VALID(fu32v, fnu32v);
	CALLABLE_VALID(fu32p, fnu32p);
	CALLABLE_VALID(fu64v, fnu64v);
	CALLABLE_VALID(fu64p, fnu64p);
	CALLABLE_VALID(fi8v, fni8v);
	CALLABLE_VALID(fi8p, fni8p);
	CALLABLE_VALID(fi16v, fni16v);
	CALLABLE_VALID(fi16p, fni16p);
	CALLABLE_VALID(fi32v, fni32v);
	CALLABLE_VALID(fi32p, fni32p);
	CALLABLE_VALID(fi64v, fni64v);
	CALLABLE_VALID(fi64p, fni64p);
	CALLABLE_VALID(fptrv, fnptrv);
	CALLABLE_VALID(fptrp, fnptrp);
	default: return 0;
	}
#undef CALLABLE_VALID
}

/* Tagged result storage: select value's member using the return type in tag.
 * Pointer results are borrowed; no ownership transfer or automatic free.
 */
struct callable_result {
	CallableType tag;
	union {
		uint8_t u8;
		uint16_t u16;
		uint32_t u32;
		uint64_t u64;
		int8_t i8;
		int16_t i16;
		int32_t i32;
		int64_t i64;
		void *ptr;
	} value;
};

/* Typed alternative to callable_call's raw output pointer. Failure preserves
 * result. NULL result accepts only void callbacks, without discarding values.
 */
static inline enum status callable_invoke(const struct callable *callable,
					 struct callable_result *result) {
	if (!callable) return FAILED;
	if (!result) return callable_call(callable, 0);
	struct callable_result fresh = { .tag = callable->tag };
	enum status status;
#define CALLABLE_RESULT(tag_value, member) \
	case tag_value: status = callable_call(callable, &fresh.value.member); break
	switch (callable->tag) {
	case fvv:
	case fvv0:
		status = callable_call(callable, 0);
		break;
	CALLABLE_RESULT(fu8v, u8);
	CALLABLE_RESULT(fu8p, u8);
	CALLABLE_RESULT(fu16v, u16);
	CALLABLE_RESULT(fu16p, u16);
	CALLABLE_RESULT(fu32v, u32);
	CALLABLE_RESULT(fu32p, u32);
	CALLABLE_RESULT(fu64v, u64);
	CALLABLE_RESULT(fu64p, u64);
	CALLABLE_RESULT(fi8v, i8);
	CALLABLE_RESULT(fi8p, i8);
	CALLABLE_RESULT(fi16v, i16);
	CALLABLE_RESULT(fi16p, i16);
	CALLABLE_RESULT(fi32v, i32);
	CALLABLE_RESULT(fi32p, i32);
	CALLABLE_RESULT(fi64v, i64);
	CALLABLE_RESULT(fi64p, i64);
	CALLABLE_RESULT(fptrv, ptr);
	CALLABLE_RESULT(fptrp, ptr);
	default: return FAILED;
	}
#undef CALLABLE_RESULT
	if (status == SUCCESS) *result = fresh;
	return status;
}

#endif
