#include "kalloc.h"
#include "ktypes.h"

#define CACHE_COUNT 8u
#define BITMAP_WORDS (BUDDY_PAGE_SIZE / KALLOC_MIN_SIZE / 64u)
#define OBJECT_ALIGNMENT _Alignof(kmax_align_t)
#define HEADER_BYTES(type) \
	((sizeof(type) + OBJECT_ALIGNMENT - 1) / OBJECT_ALIGNMENT * OBJECT_ALIGNMENT)

struct free_object {
	struct free_object *next;
};

/* Metadata is inside the slab page, before the aligned object area. The
 * bitmap tracks live slots independently of the free links in unused slots.
 */
struct slab {
	struct node node;
	struct free_object *free;
	ksize_t free_count;
	u64_t allocated[BITMAP_WORDS];
};

struct cache {
	struct list_meta partial;
	struct list_meta full;
};

struct large_allocation {
	struct node node;
	struct block backing;
};

_Static_assert(KALLOC_MIN_SIZE % OBJECT_ALIGNMENT == 0,
	       "slab classes must align kmax_align_t");
_Static_assert(sizeof(struct free_object) <= KALLOC_MIN_SIZE,
	       "free link must fit in the smallest class");
_Static_assert(HEADER_BYTES(struct slab) + KALLOC_SLAB_MAX_SIZE <= BUDDY_PAGE_SIZE,
	       "largest slab object must fit in a page");

static struct buddy_allocator *backing;
static struct cache caches[CACHE_COUNT];
static struct list_meta large_allocations;

enum kalloc_status kalloc_init(struct buddy_allocator *allocator) {
	if (allocator == NULL || allocator->pages == NULL ||
	    allocator->page_count == 0)
		return KALLOC_INVALID_ARGUMENT;
	if (backing != NULL) {
		if (!list_empty(&large_allocations))
			return KALLOC_BUSY;
		for (ksize_t i = 0; i < CACHE_COUNT; i++)
			if (!list_empty(&caches[i].partial) ||
			    !list_empty(&caches[i].full))
				return KALLOC_BUSY;
	}
	for (ksize_t i = 0; i < CACHE_COUNT; i++) {
		list_init(&caches[i].partial);
		list_init(&caches[i].full);
	}
	list_init(&large_allocations);
	backing = allocator;
	return KALLOC_OK;
}

static ksize_t slab_capacity(ksize_t object_size) {
	return (BUDDY_PAGE_SIZE - HEADER_BYTES(struct slab)) / object_size;
}

static uptr_t slab_objects(const struct slab *slab) {
	return (uptr_t)slab + HEADER_BYTES(struct slab);
}

static struct slab *slab_create(struct cache *cache, ksize_t object_size) {
	struct block block;
	if (buddy_alloc(backing, 0, &block) != BUDDY_OK)
		return NULL;
	struct slab *slab = (void *)block.addr;
	node_init(&slab->node, slab);
	slab->free_count = slab_capacity(object_size);
	slab->free = NULL;
	for (ksize_t i = 0; i < BITMAP_WORDS; i++)
		slab->allocated[i] = 0;
	/* Reverse insertion makes the first allocation the lowest-address slot. */
	for (ksize_t i = slab->free_count; i > 0; i--) {
		struct free_object *object =
			(void *)(slab_objects(slab) + (i - 1) * object_size);
		object->next = slab->free;
		slab->free = object;
	}
	list_push_front(&cache->partial, &slab->node);
	return slab;
}

static void *alloc_large(ksize_t size) {
	ksize_t header = HEADER_BYTES(struct large_allocation);
	if (size > BUDDY_BLOCK_BYTES(BUDDY_MAX_ORDER) - header)
		return NULL;
	ksize_t bytes = size + header;
	int order = 0;
	while (BUDDY_BLOCK_BYTES(order) < bytes)
		order++;
	struct block block;
	if (buddy_alloc(backing, order, &block) != BUDDY_OK)
		return NULL;
	struct large_allocation *allocation = (void *)block.addr;
	allocation->backing = block;
	node_init(&allocation->node, allocation);
	list_push_front(&large_allocations, &allocation->node);
	return (void *)(block.addr + header);
}

void *kalloc(ksize_t size) {
	if (backing == NULL || size == 0)
		return NULL;
	if (size > KALLOC_SLAB_MAX_SIZE)
		return alloc_large(size);
	ksize_t index = 0;
	ksize_t object_size = KALLOC_MIN_SIZE;
	while (object_size < size) {
		object_size <<= 1;
		index++;
	}
	struct cache *cache = &caches[index];
	struct node *node = list_front(&cache->partial);
	struct slab *slab = node != NULL ? node->data : slab_create(cache, object_size);
	if (slab == NULL)
		return NULL;
	struct free_object *object = slab->free;
	slab->free = object->next;
	ksize_t slot = ((uptr_t)object - slab_objects(slab)) / object_size;
	slab->allocated[slot / 64] |= U64_C(1) << (slot % 64);
	if (--slab->free_count == 0) {
		list_remove(&cache->partial, &slab->node);
		list_push_front(&cache->full, &slab->node);
	}
	return object;
}

static struct slab *find_slab(const struct list_meta *list, uptr_t page) {
	for (struct node *node = list->head; node != NULL; node = node->next)
		if ((uptr_t)node->data == page)
			return node->data;
	return NULL;
}

static enum kalloc_status free_small(struct cache *cache, struct slab *slab,
				   ksize_t object_size, uptr_t address) {
	uptr_t objects = slab_objects(slab);
	ksize_t capacity = slab_capacity(object_size);
	if (address < objects || address - objects >= capacity * object_size ||
	    (address - objects) % object_size != 0)
		return KALLOC_NOT_ALLOCATED;
	ksize_t slot = (address - objects) / object_size;
	u64_t mask = U64_C(1) << (slot % 64);
	if ((slab->allocated[slot / 64] & mask) == 0)
		return KALLOC_NOT_ALLOCATED;
	slab->allocated[slot / 64] &= ~mask;
	struct free_object *object = (void *)address;
	object->next = slab->free;
	slab->free = object;
	if (slab->free_count++ == 0) {
		list_remove(&cache->full, &slab->node);
		list_push_front(&cache->partial, &slab->node);
	}
	if (slab->free_count == capacity) {
		struct block block = {(uptr_t)slab, 0};
		list_remove(&cache->partial, &slab->node);
		(void)buddy_free(backing, &block);
	}
	return KALLOC_OK;
}

enum kalloc_status kfree(void *ptr) {
	if (ptr == NULL)
		return KALLOC_OK;
	if (backing == NULL)
		return KALLOC_NOT_ALLOCATED;
	uptr_t address = (uptr_t)ptr;
	uptr_t page = address - address % BUDDY_PAGE_SIZE;
	/* Search owned metadata before dereferencing anything derived from ptr. */
	for (ksize_t i = 0; i < CACHE_COUNT; i++) {
		struct slab *slab = find_slab(&caches[i].partial, page);
		if (slab == NULL)
			slab = find_slab(&caches[i].full, page);
		if (slab != NULL)
			return free_small(&caches[i], slab, (ksize_t)KALLOC_MIN_SIZE << i,
					  address);
	}
	for (struct node *node = large_allocations.head; node != NULL; node = node->next) {
		struct large_allocation *allocation = node->data;
		if (address == allocation->backing.addr + HEADER_BYTES(struct large_allocation)) {
			struct block block = allocation->backing;
			list_remove(&large_allocations, node);
			(void)buddy_free(backing, &block);
			return KALLOC_OK;
		}
	}
	return KALLOC_NOT_ALLOCATED;
}
