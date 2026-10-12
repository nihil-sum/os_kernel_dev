#include "buddy_mm.h"
#include "ktypes.h"

/* Validate before shifting or subtracting, so malformed descriptors cannot
 * cause undefined shifts, integer wraparound, or out-of-arena results.
 */
static enum buddy_status validate_arena(const struct buddy_arena *arena) {
	if (arena == NULL || arena->size == 0 ||
	    arena->base % BUDDY_PAGE_SIZE != 0 ||
	    arena->size % BUDDY_PAGE_SIZE != 0) {
		return BUDDY_INVALID_ARGUMENT;
	}
	if (arena->size > UPTR_MAX - arena->base) {
		return BUDDY_OUT_OF_RANGE;
	}
	return BUDDY_OK;
}

static enum buddy_status validate_block(const struct buddy_arena *arena,
					const struct block *block,
					ksize_t *bytes, uptr_t *offset) {
	if (block == NULL) {
		return BUDDY_INVALID_ARGUMENT;
	}
	enum buddy_status status = validate_arena(arena);
	if (status != BUDDY_OK) {
		return status;
	}
	if (block->order < 0 || block->order > BUDDY_MAX_ORDER ||
	    (unsigned int)block->order >= sizeof(ksize_t) * K_CHAR_BIT ||
	    (KSIZE_MAX >> block->order) < BUDDY_PAGE_SIZE) {
		return BUDDY_INVALID_ORDER;
	}

	*bytes = BUDDY_BLOCK_BYTES(block->order);
	uptr_t address = block->addr;
	if (address < arena->base) {
		return BUDDY_OUT_OF_RANGE;
	}
	*offset = address - arena->base;
	if (*offset % *bytes != 0) {
		return BUDDY_UNALIGNED;
	}
	if (*bytes > arena->size || *offset > arena->size - *bytes) {
		return BUDDY_OUT_OF_RANGE;
	}
	return BUDDY_OK;
}

enum buddy_status buddy_init(struct buddy_allocator *allocator,
			     const struct buddy_arena *arena,
			     struct buddy_page *pages, ksize_t page_capacity) {
	if (allocator == NULL || pages == NULL) {
		return BUDDY_INVALID_ARGUMENT;
	}
	enum buddy_status status = validate_arena(arena);
	if (status != BUDDY_OK) {
		return status;
	}
	ksize_t count = arena->size / BUDDY_PAGE_SIZE;
	if (count > page_capacity || count > KSIZE_MAX / sizeof(*pages)) {
		return BUDDY_METADATA_TOO_SMALL;
	}

	allocator->arena = *arena;
	allocator->pages = pages;
	allocator->page_count = count;
	for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
		list_init(&allocator->free_lists[order]);
	}
	for (ksize_t i = 0; i < count; i++) {
		pages[i].state = BUDDY_PAGE_INTERIOR;
		pages[i].order = -1;
		node_init(&pages[i].free_node, &pages[i]);
	}
	for (ksize_t index = 0; index < count;) {
		int order = BUDDY_MAX_ORDER;
		ksize_t block_pages = (ksize_t)1 << order;
		while (block_pages > count - index ||
		       index % block_pages != 0) {
			order--;
			block_pages >>= 1;
		}
		pages[index].state = BUDDY_PAGE_FREE;
		pages[index].order = order;
		list_push_back(&allocator->free_lists[order],
			       &pages[index].free_node);
		index += block_pages;
	}
	return BUDDY_OK;
}

struct buddy_page *buddy_page_at(const struct buddy_allocator *allocator,
				 uptr_t addr) {
	if (allocator == NULL || allocator->pages == NULL ||
	    addr < allocator->arena.base) {
		return NULL;
	}
	uptr_t offset = addr - allocator->arena.base;
	if (offset >= allocator->arena.size || offset % BUDDY_PAGE_SIZE != 0) {
		return NULL;
	}
	return &allocator->pages[offset / BUDDY_PAGE_SIZE];
}

enum buddy_status buddy_alloc(struct buddy_allocator *allocator, int order,
                             struct block *block) {
        if (allocator == NULL || allocator->pages == NULL || block == NULL)
                return BUDDY_INVALID_ARGUMENT;
        if (order < 0 || order > BUDDY_MAX_ORDER)
                return BUDDY_INVALID_ORDER;
        int found = order;
        while (found <= BUDDY_MAX_ORDER &&
               list_empty(&allocator->free_lists[found]))
                found++;
        if (found > BUDDY_MAX_ORDER)
                return BUDDY_NO_MEMORY;
        struct node *node = list_pop_front(&allocator->free_lists[found]);
        struct buddy_page *page = node->data;
        ksize_t index = (ksize_t)(page - allocator->pages);
        while (found > order) {
                found--;
                struct buddy_page *right =
                        &allocator->pages[index + ((ksize_t)1 << found)];
                right->state = BUDDY_PAGE_FREE;
                right->order = found;
                list_push_front(&allocator->free_lists[found], &right->free_node);
        }
        page->state = BUDDY_PAGE_ALLOCATED;
        page->order = order;
        *block = (struct block){allocator->arena.base + index * BUDDY_PAGE_SIZE,
                                order};
        return BUDDY_OK;
}

enum buddy_status buddy_free(struct buddy_allocator *allocator,
                            const struct block *block) {
        if (allocator == NULL || allocator->pages == NULL)
                return BUDDY_INVALID_ARGUMENT;
        ksize_t bytes;
        uptr_t offset;
        enum buddy_status status =
                validate_block(&allocator->arena, block, &bytes, &offset);
        if (status != BUDDY_OK)
                return status;
        ksize_t index = offset / BUDDY_PAGE_SIZE;
        struct buddy_page *page = &allocator->pages[index];
        if (page->state != BUDDY_PAGE_ALLOCATED || page->order != block->order)
                return BUDDY_NOT_ALLOCATED;
        int order = block->order;
        while (order < BUDDY_MAX_ORDER) {
                ksize_t other = index ^ ((ksize_t)1 << order);
                if (other >= allocator->page_count)
                        break;
                struct buddy_page *buddy = &allocator->pages[other];
                if (buddy->state != BUDDY_PAGE_FREE || buddy->order != order)
                        break;
                list_remove(&allocator->free_lists[order], &buddy->free_node);
                ksize_t upper = index > other ? index : other;
                allocator->pages[upper].state = BUDDY_PAGE_INTERIOR;
                allocator->pages[upper].order = -1;
                index = index < other ? index : other;
                order++;
        }
        page = &allocator->pages[index];
        page->state = BUDDY_PAGE_FREE;
        page->order = order;
        list_push_front(&allocator->free_lists[order], &page->free_node);
        return BUDDY_OK;
}

enum buddy_status buddy_get_buddy(const struct buddy_arena *arena,
				  const struct block *block,
				  struct block *buddy) {
	if (buddy == NULL) {
		return BUDDY_INVALID_ARGUMENT;
	}
	ksize_t bytes;
	uptr_t offset;
	enum buddy_status status =
		validate_block(arena, block, &bytes, &offset);
	if (status != BUDDY_OK) {
		return status;
	}
	uptr_t buddy_offset = offset ^ (uptr_t)bytes;
	if (buddy_offset > arena->size - bytes) {
		return BUDDY_OUT_OF_RANGE;
	}
	struct block result = {.addr = arena->base + buddy_offset,
			       .order = block->order};
	*buddy = result;
	return BUDDY_OK;
}

enum buddy_status buddy_split(const struct buddy_arena *arena,
			      const struct block *parent, struct block *left,
			      struct block *right) {
	if (left == NULL || right == NULL || left == right) {
		return BUDDY_INVALID_ARGUMENT;
	}
	ksize_t bytes;
	uptr_t offset;
	enum buddy_status status =
		validate_block(arena, parent, &bytes, &offset);
	if (status != BUDDY_OK) {
		return status;
	}
	if (parent->order == 0) {
		return BUDDY_INVALID_ORDER;
	}
	struct block lower = {.addr = parent->addr, .order = parent->order - 1};
	struct block upper = {.addr = arena->base + offset + bytes / 2,
			      .order = parent->order - 1};
	*left = lower;
	*right = upper;
	return BUDDY_OK;
}

enum buddy_status buddy_combine(const struct buddy_arena *arena,
				const struct block *first,
				const struct block *second,
				struct block *parent) {
	if (parent == NULL) {
		return BUDDY_INVALID_ARGUMENT;
	}
	ksize_t first_bytes, second_bytes;
	uptr_t first_offset, second_offset;
	enum buddy_status status =
		validate_block(arena, first, &first_bytes, &first_offset);
	if (status != BUDDY_OK) {
		return status;
	}
	status = validate_block(arena, second, &second_bytes, &second_offset);
	if (status != BUDDY_OK) {
		return status;
	}
	if (first->order != second->order ||
	    (first_offset ^ (uptr_t)first_bytes) != second_offset) {
		return BUDDY_NOT_BUDDIES;
	}
	if (first->order == BUDDY_MAX_ORDER) {
		return BUDDY_INVALID_ORDER;
	}
	struct block result = {.addr = first_offset < second_offset
					       ? first->addr
					       : second->addr,
			       .order = first->order + 1};
	/* Also check that the larger order is representable on this platform.
	 */
	status = validate_block(arena, &result, &first_bytes, &first_offset);
	if (status != BUDDY_OK) {
		return status;
	}
	*parent = result;
	return BUDDY_OK;
}
