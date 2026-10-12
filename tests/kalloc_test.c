#include "kalloc.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGES 64u
_Alignas(BUDDY_PAGE_SIZE) static unsigned char memory[TEST_PAGES * BUDDY_PAGE_SIZE];
static struct buddy_page metadata[TEST_PAGES];
static struct buddy_allocator allocator;

static ksize_t free_pages(void) {
	ksize_t count = 0;
	for (int i = 0; i <= BUDDY_MAX_ORDER; i++)
		count += allocator.free_lists[i].len * ((ksize_t)1 << i);
	return count;
}

static void reset(ksize_t pages) {
	struct buddy_arena arena = {(uptr_t)memory, pages * BUDDY_PAGE_SIZE};
	assert(buddy_init(&allocator, &arena, metadata, TEST_PAGES) == BUDDY_OK);
	assert(kalloc_init(&allocator) == KALLOC_OK);
	assert(free_pages() == pages);
}

static void verify_bytes(const unsigned char *ptr, ksize_t size, unsigned char byte) {
	for (ksize_t i = 0; i < size; i++)
		assert(ptr[i] == byte);
}

static void initialization_checks(void) {
	struct buddy_allocator invalid = {0};
	assert(kalloc(1) == NULL);
	assert(kfree(NULL) == KALLOC_OK);
	assert(kfree(memory) == KALLOC_NOT_ALLOCATED);
	assert(kalloc_init(NULL) == KALLOC_INVALID_ARGUMENT);
	assert(kalloc_init(&invalid) == KALLOC_INVALID_ARGUMENT);
	reset(TEST_PAGES);
	assert(kalloc(0) == NULL);
	assert(kalloc(KSIZE_MAX) == NULL);
	assert(kalloc(BUDDY_BLOCK_BYTES(BUDDY_MAX_ORDER)) == NULL);
	assert(free_pages() == TEST_PAGES);
	void *small = kalloc(32);
	assert(small != NULL);
	assert(kalloc_init(&allocator) == KALLOC_BUSY);
	assert(kfree(small) == KALLOC_OK);
	void *large = kalloc(4096);
	assert(large != NULL);
	assert(kalloc_init(&allocator) == KALLOC_BUSY);
	assert(kfree(large) == KALLOC_OK);
	assert(kalloc_init(&allocator) == KALLOC_OK);
	assert(free_pages() == TEST_PAGES);
}

static void size_checks(void) {
	const ksize_t sizes[] = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
		129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048,
		2049, 4000, 4095, 4096, 4097, 8192, 16385, 65536};
	reset(TEST_PAGES);
	for (ksize_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		unsigned char *a = kalloc(sizes[i]);
		unsigned char *b = kalloc(sizes[i]);
		assert(a != NULL && b != NULL && a != b);
		assert((uptr_t)a % _Alignof(kmax_align_t) == 0);
		assert((uptr_t)b % _Alignof(kmax_align_t) == 0);
		if (sizes[i] <= 1024)
			assert(free_pages() == TEST_PAGES - 1);
		memset(a, 0xa5, sizes[i]);
		memset(b, 0x5a, sizes[i]);
		verify_bytes(a, sizes[i], 0xa5);
		verify_bytes(b, sizes[i], 0x5a);
		assert(kfree(a) == KALLOC_OK);
		verify_bytes(b, sizes[i], 0x5a);
		assert(kfree(b) == KALLOC_OK);
		assert(kfree(a) == KALLOC_NOT_ALLOCATED);
		assert(kfree(b) == KALLOC_NOT_ALLOCATED);
		assert(free_pages() == TEST_PAGES);
		assert(allocator.free_lists[6].len == 1);
	}
}

static void invalid_free_checks(void) {
	reset(TEST_PAGES);
	unsigned char *a = kalloc(32);
	unsigned char *b = kalloc(32);
	unsigned char *large = kalloc(5000);
	assert(a != NULL && b != NULL && large != NULL);
	memset(a, 0xa5, 32);
	memset(large, 0x5a, 5000);
	ksize_t before = free_pages();
	struct block foreign;
	assert(buddy_alloc(&allocator, 0, &foreign) == BUDDY_OK);
	assert(kfree((void *)foreign.addr) == KALLOC_NOT_ALLOCATED);
	assert(kfree((void *)(uptr_t)1) == KALLOC_NOT_ALLOCATED);
	assert(kfree((void *)UPTR_MAX) == KALLOC_NOT_ALLOCATED);
	assert(kfree(&before) == KALLOC_NOT_ALLOCATED);
	assert(kfree(a + 1) == KALLOC_NOT_ALLOCATED);
	assert(kfree(a - 1) == KALLOC_NOT_ALLOCATED);
	assert(kfree(memory) == KALLOC_NOT_ALLOCATED); /* slab header */
	assert(kfree(large + 1) == KALLOC_NOT_ALLOCATED);
	assert(kfree(large + BUDDY_PAGE_SIZE) == KALLOC_NOT_ALLOCATED);
	assert(kfree(large - 1) == KALLOC_NOT_ALLOCATED);
	assert(kfree(NULL) == KALLOC_OK);
	verify_bytes(a, 32, 0xa5);
	verify_bytes(large, 5000, 0x5a);
	assert(buddy_free(&allocator, &foreign) == BUDDY_OK);
	assert(free_pages() == before);
	assert(kfree(a) == KALLOC_OK);
	assert(kfree(a) == KALLOC_NOT_ALLOCATED); /* still-owned partial slab */
	assert(kfree(b) == KALLOC_OK);
	assert(kfree(large) == KALLOC_OK);
	assert(free_pages() == TEST_PAGES);
}

static void exhaustion_checks(void) {
	/* Force multiple full slabs, then reuse holes without any free pages. */
	const ksize_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
	void *objects[4 * BUDDY_PAGE_SIZE / KALLOC_MIN_SIZE];
	for (ksize_t c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++) {
		reset(4);
		ksize_t count = 0;
		void *ptr;
		while ((ptr = kalloc(sizes[c])) != NULL) {
			assert(count < sizeof(objects) / sizeof(objects[0]));
			objects[count++] = ptr;
			memset(ptr, (unsigned char)(count % 251), sizes[c]);
		}
		assert(count >= 4 && free_pages() == 0);
		assert(kalloc(sizes[c]) == NULL);
		assert(kalloc(4096) == NULL);
		for (ksize_t i = 0; i < count; i++)
			verify_bytes(objects[i], sizes[c], (unsigned char)((i + 1) % 251));
		for (ksize_t i = 0; i < count; i += 2)
			assert(kfree(objects[i]) == KALLOC_OK);
		for (ksize_t i = 0; i < count; i += 2) {
			objects[i] = kalloc(sizes[c]);
			assert(objects[i] != NULL);
		}
		assert(kalloc(sizes[c]) == NULL);
		for (ksize_t i = 1; i < count; i += 2)
			verify_bytes(objects[i], sizes[c], (unsigned char)((i + 1) % 251));
		for (ksize_t i = count; i > 0; i--)
			assert(kfree(objects[i - 1]) == KALLOC_OK);
		assert(free_pages() == 4 && allocator.free_lists[2].len == 1);
	}
	reset(1);
	assert(kalloc(4096) == NULL); /* header requires a second page */
	assert(free_pages() == 1);
	void *ptr = kalloc(2049);
	assert(ptr != NULL && free_pages() == 0);
	assert(kalloc(16) == NULL);
	assert(kfree(ptr) == KALLOC_OK);
	ptr = kalloc(16);
	assert(ptr != NULL && kfree(ptr) == KALLOC_OK);
	assert(free_pages() == 1);
}

static void mixed_checks(void) {
	/* Deterministic churn checks mixed caches, full/partial transitions and
	 * large allocations while other objects retain arbitrary contents.
	 */
	unsigned char *objects[48] = {0};
	ksize_t sizes[48] = {0};
	u32_t random = 42;
	reset(TEST_PAGES);
	for (ksize_t step = 0; step < 5000; step++) {
		random = random * U32_C(1664525) + U32_C(1013904223);
		ksize_t slot = (random >> 16) % 48;
		if (objects[slot] != NULL) {
			verify_bytes(objects[slot], sizes[slot], (unsigned char)(slot + 1));
			assert(kfree(objects[slot]) == KALLOC_OK);
			objects[slot] = NULL;
		} else {
			sizes[slot] = (random >> 8) % 9000 + 1;
			objects[slot] = kalloc(sizes[slot]);
			if (objects[slot] != NULL)
				memset(objects[slot], (unsigned char)(slot + 1), sizes[slot]);
		}
	}
	for (ksize_t i = 0; i < 48; i++) {
		if (objects[i] != NULL)
			verify_bytes(objects[i], sizes[i], (unsigned char)(i + 1));
		assert(kfree(objects[i]) == KALLOC_OK);
	}
	assert(free_pages() == TEST_PAGES && allocator.free_lists[6].len == 1);
}

int main(void) {
	initialization_checks();
	size_checks();
	invalid_free_checks();
	exhaustion_checks();
	mixed_checks();
	puts("KALLOC OK: slab classes, alignment, reuse, invalid frees, exhaustion, large blocks, coalescing");
	return 0;
}
