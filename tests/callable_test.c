#include "types.h"
#include "callable.h"
#include <assert.h>
#include <stddef.h>

static unsigned int calls;

static void increment(void *arg) {
	++*(unsigned int *)arg;
}

static void accept_null(void *arg) {
	assert(arg == NULL);
	++calls;
}

static u64_t return_value(void) {
	++calls;
	return 0xfedcba9876543210ULL;
}

static unsigned int extra_calls;

static void no_args(void) {
	++extra_calls;
}

/* Exercise exact-width writes, signed boundaries, and both argument forms. */
#define DEFINE_INTEGER_TEST(name, type, constant) \
	static type name##_value(void) { \
		++extra_calls; \
		return constant; \
	} \
	static type name##_context(void *arg) { \
		++extra_calls; \
		return *(type *)arg; \
	} \
	static void test_##name(void) { \
		struct { type before, result, after; } storage = { 7, 0, 9 }; \
		type expected = constant; \
		unsigned int previous = extra_calls; \
		struct callable cb = callable_from_fn##name##v(name##_value); \
		assert(callable_call(&cb, NULL) == FAILED); \
		assert(extra_calls == previous); \
		assert(callable_call(&cb, &storage.result) == SUCCESS); \
		assert(storage.result == expected); \
		cb = callable_from_fn##name##p(name##_context, &expected); \
		assert(callable_call(&cb, NULL) == FAILED); \
		assert(callable_call(&cb, &storage.result) == SUCCESS); \
		assert(storage.result == expected); \
		assert(storage.before == 7 && storage.after == 9); \
		assert(extra_calls == previous + 2); \
		cb = callable_from_fn##name##v(NULL); \
		assert(callable_call(&cb, &storage.result) == FAILED); \
		cb = callable_from_fn##name##p(NULL, &expected); \
		assert(callable_call(&cb, &storage.result) == FAILED); \
		assert(storage.result == expected && extra_calls == previous + 2); \
		assert(!callable_valid(&cb)); \
		struct callable_result typed = { .tag = fptrv, .value.ptr = &calls }; \
		assert(callable_invoke(&cb, &typed) == FAILED); \
		assert(typed.tag == fptrv && typed.value.ptr == &calls); \
		cb = callable_from_fn##name##p(name##_context, &expected); \
		assert(callable_valid(&cb)); \
		assert(callable_invoke(&cb, NULL) == FAILED); \
		assert(callable_invoke(&cb, &typed) == SUCCESS); \
		assert(typed.tag == cb.tag && typed.value.name == expected); \
		cb = callable_from_fn##name##v(name##_value); \
		assert(callable_invoke(&cb, &typed) == SUCCESS); \
		assert(typed.tag == cb.tag && typed.value.name == expected); \
	}

DEFINE_INTEGER_TEST(u8, u8_t, 255)
DEFINE_INTEGER_TEST(u16, u16_t, 65535)
DEFINE_INTEGER_TEST(u32, u32_t, 0xffffffffU)
DEFINE_INTEGER_TEST(u64, u64_t, 0xffffffffffffffffULL)
DEFINE_INTEGER_TEST(i8, i8_t, -128)
DEFINE_INTEGER_TEST(i16, i16_t, -32768)
DEFINE_INTEGER_TEST(i32, i32_t, (-2147483647 - 1))
DEFINE_INTEGER_TEST(i64, i64_t, (-9223372036854775807LL - 1))

#undef DEFINE_INTEGER_TEST

static void *pointer_value(void) {
	++extra_calls;
	return &extra_calls;
}

static void *pointer_context(void *arg) {
	++extra_calls;
	return arg;
}

static void test_additional_types(void) {
	test_u8();
	test_u16();
	test_u32();
	test_u64();
	test_i8();
	test_i16();
	test_i32();
	test_i64();

	struct callable cb = callable_from_fnvv0(no_args);
	unsigned int previous = extra_calls;
	assert(callable_call(&cb, NULL) == SUCCESS);
	assert(extra_calls == previous + 1);
	cb = callable_from_fnvv0(NULL);
	assert(callable_call(&cb, NULL) == FAILED);

	void *result = NULL;
	cb = callable_from_fnptrv(pointer_value);
	assert(callable_call(&cb, NULL) == FAILED);
	assert(callable_call(&cb, &result) == SUCCESS);
	assert(result == &extra_calls);
	cb = callable_from_fnptrp(pointer_context, &calls);
	assert(callable_call(&cb, NULL) == FAILED);
	assert(callable_call(&cb, &result) == SUCCESS);
	assert(result == &calls);
	cb = callable_from_fnptrp(pointer_context, NULL);
	assert(callable_call(&cb, &result) == SUCCESS);
	assert(result == NULL && extra_calls == previous + 4);
	cb = callable_from_fnptrv(NULL);
	assert(callable_call(&cb, &result) == FAILED);
	cb = callable_from_fnptrp(NULL, NULL);
	assert(callable_call(&cb, &result) == FAILED);
	struct callable_result typed;
	cb = callable_from_fnptrv(pointer_value);
	assert(callable_valid(&cb) && callable_invoke(&cb, &typed) == SUCCESS);
	assert(typed.tag == fptrv && typed.value.ptr == &extra_calls);
	cb = callable_from_fnptrp(pointer_context, &calls);
	assert(callable_invoke(&cb, &typed) == SUCCESS);
	assert(typed.tag == fptrp && typed.value.ptr == &calls);
	cb = callable_from_fnvv0(no_args);
	assert(callable_invoke(&cb, &typed) == SUCCESS && typed.tag == fvv0);
	assert(callable_invoke(&cb, NULL) == SUCCESS);
}

int main(void) {
	test_additional_types();
	u64_t result = 42;
	unsigned int count = 0;
	struct callable callback = callable_from_fnv(increment, &count);
	assert(callable_call(&callback, &result) == SUCCESS);
	assert(count == 1 && result == 42);
	assert(callable_call(&callback, NULL) == SUCCESS);
	assert(count == 2);

	callback = callable_from_fnv(accept_null, NULL);
	assert(callable_call(&callback, NULL) == SUCCESS);
	assert(calls == 1);

	callback = callable_from_fnu64v(return_value);
	assert(callable_call(&callback, NULL) == FAILED);
	assert(calls == 1);
	assert(callable_call(&callback, &result) == SUCCESS);
	assert(calls == 2 && result == 0xfedcba9876543210ULL);

	callback = callable_from_fnv(NULL, NULL);
	assert(callable_call(&callback, &result) == FAILED);
	callback = callable_from_fnu64v(NULL);
	assert(callable_call(&callback, &result) == FAILED);
	callback = (struct callable){0};
	assert(callable_call(&callback, &result) == FAILED);
	callback.tag = (CallableType)99;
	assert(callable_call(&callback, &result) == FAILED);
	assert(callable_call(NULL, &result) == FAILED);
	assert(!callable_valid(&callback) && !callable_valid(NULL));
	struct callable_result typed = { .tag = fu64v, .value.u64 = 42 };
	assert(callable_invoke(NULL, &typed) == FAILED);
	assert(callable_invoke(&callback, &typed) == FAILED);
	assert(typed.tag == fu64v && typed.value.u64 == 42);
	callback = callable_from_fnv(increment, &count);
	assert(callable_valid(&callback));
	assert(callable_invoke(&callback, &typed) == SUCCESS);
	assert(count == 3 && typed.tag == fvv);
	assert(calls == 2 && result == 0xfedcba9876543210ULL);
	return 0;
}
