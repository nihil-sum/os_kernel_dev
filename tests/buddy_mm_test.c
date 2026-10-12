#include "buddy_mm.h"
#include "mm.h" /* Both allocator headers must coexist. */
#include <assert.h>
#include <string.h>

static struct block block_at(uptr_t address, int order) {
	struct block block = {.addr = address, .order = order};
	return block;
}

static void same_block(struct block actual, struct block expected) {
	assert(actual.addr == expected.addr);
	assert(actual.order == expected.order);
}

static void round_trips(const struct buddy_arena *arena) {
	for (int order = 1; order <= BUDDY_MAX_ORDER; order++) {
		ksize_t bytes = (ksize_t)BUDDY_PAGE_SIZE << order;
		/* Check both ends, with a base that is not aligned to larger
		 * orders. */
		uptr_t addresses[] = {arena->base,
					 arena->base + arena->size - bytes};
		for (ksize_t i = 0; i < 2; i++) {
			struct block original = block_at(addresses[i], order);
			struct block left, right, buddy, parent;
			assert(buddy_split(arena, &original, &left, &right) ==
			       BUDDY_OK);
			same_block(left, block_at(addresses[i], order - 1));
			same_block(right, block_at(addresses[i] + bytes / 2,
						   order - 1));
			assert(buddy_get_buddy(arena, &left, &buddy) ==
			       BUDDY_OK);
			same_block(buddy, right);
			assert(buddy_get_buddy(arena, &right, &buddy) ==
			       BUDDY_OK);
			same_block(buddy, left);
			assert(buddy_combine(arena, &left, &right, &parent) ==
			       BUDDY_OK);
			same_block(parent, original);
			assert(buddy_combine(arena, &right, &left, &parent) ==
			       BUDDY_OK);
			same_block(parent, original);

			/* Reusing input descriptors is supported in either
			 * output position. */
			assert(buddy_split(arena, &parent, &parent, &right) ==
			       BUDDY_OK);
			same_block(parent, left);
			assert(buddy_combine(arena, &parent, &right, &right) ==
			       BUDDY_OK);
			same_block(right, original);
			assert(buddy_split(arena, &right, &left, &right) ==
			       BUDDY_OK);
			assert(buddy_get_buddy(arena, &left, &left) ==
			       BUDDY_OK);
			same_block(left, right);
		}
	}
}

static void failures(const struct buddy_arena *arena) {
	struct block sentinel = block_at(0xdead000, 7);
	struct block left = sentinel, right = sentinel;
	struct block input = block_at(arena->base, 0);
	assert(buddy_split(arena, &input, &left, &right) ==
	       BUDDY_INVALID_ORDER);
	same_block(left, sentinel);
	same_block(right, sentinel);

	input.order = -1;
	assert(buddy_get_buddy(arena, &input, &left) == BUDDY_INVALID_ORDER);
	input.order = BUDDY_MAX_ORDER + 1;
	assert(buddy_split(arena, &input, &left, &right) ==
	       BUDDY_INVALID_ORDER);
	input.order = 1000;
	assert(buddy_get_buddy(arena, &input, &left) == BUDDY_INVALID_ORDER);

	input = block_at(arena->base + BUDDY_PAGE_SIZE, 1);
	assert(buddy_split(arena, &input, &left, &right) == BUDDY_UNALIGNED);
	input = block_at(arena->base - BUDDY_PAGE_SIZE, 0);
	assert(buddy_get_buddy(arena, &input, &left) == BUDDY_OUT_OF_RANGE);
	input = block_at(arena->base + arena->size, 0);
	assert(buddy_get_buddy(arena, &input, &left) == BUDDY_OUT_OF_RANGE);

	input = block_at(arena->base, 1);
	assert(buddy_split(arena, &input, &left, &left) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_split(arena, &input, NULL, &right) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_split(arena, &input, &left, NULL) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_split(arena, NULL, &left, &right) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_get_buddy(NULL, &input, &left) == BUDDY_INVALID_ARGUMENT);
	assert(buddy_get_buddy(arena, &input, NULL) == BUDDY_INVALID_ARGUMENT);
	assert(buddy_combine(arena, &input, &input, NULL) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_combine(arena, &input, NULL, &left) ==
	       BUDDY_INVALID_ARGUMENT);

	/* Adjacent pages 1 and 2 have different parents, so cannot combine. */
	struct block first = block_at(arena->base + BUDDY_PAGE_SIZE, 0);
	struct block second = block_at(arena->base + 2 * BUDDY_PAGE_SIZE, 0);
	assert(buddy_combine(arena, &first, &second, &left) ==
	       BUDDY_NOT_BUDDIES);
	assert(buddy_combine(arena, &first, &first, &left) ==
	       BUDDY_NOT_BUDDIES);
	first = block_at(arena->base, 1);
	assert(buddy_combine(arena, &first, &second, &left) ==
	       BUDDY_NOT_BUDDIES);

	first = block_at(arena->base, BUDDY_MAX_ORDER);
	second = block_at(arena->base +
				  ((ksize_t)BUDDY_PAGE_SIZE << BUDDY_MAX_ORDER),
			  BUDDY_MAX_ORDER);
	assert(buddy_combine(arena, &first, &second, &left) ==
	       BUDDY_INVALID_ORDER);

	struct buddy_arena small = {.base = arena->base,
				    .size = 3 * BUDDY_PAGE_SIZE};
	input = block_at(small.base + 2 * BUDDY_PAGE_SIZE, 0);
	assert(buddy_get_buddy(&small, &input, &left) == BUDDY_OUT_OF_RANGE);
	input.order = 1;
	assert(buddy_split(&small, &input, &left, &right) ==
	       BUDDY_OUT_OF_RANGE);
	input = block_at(small.base, 1);
	assert(buddy_get_buddy(&small, &input, &left) == BUDDY_OUT_OF_RANGE);

	struct buddy_arena bad = *arena;
	bad.base++;
	assert(buddy_get_buddy(&bad, &input, &left) == BUDDY_INVALID_ARGUMENT);
	bad = *arena;
	bad.size--;
	assert(buddy_get_buddy(&bad, &input, &left) == BUDDY_INVALID_ARGUMENT);
	bad.size = 0;
	assert(buddy_get_buddy(&bad, &input, &left) == BUDDY_INVALID_ARGUMENT);
	bad.base = UPTR_MAX - (BUDDY_PAGE_SIZE - 1);
	bad.size = BUDDY_PAGE_SIZE;
	input = block_at(bad.base, 0);
	assert(buddy_get_buddy(&bad, &input, &left) == BUDDY_OUT_OF_RANGE);
	same_block(left, sentinel);
	same_block(right, sentinel);
}

static void initialization(void) {
	enum { CAPACITY = (1 << (BUDDY_MAX_ORDER + 1)) + 3 };
	static struct buddy_page pages[CAPACITY + 1];
	static unsigned char covered[CAPACITY];
	struct buddy_allocator allocator;
	const ksize_t counts[] = {
		1, 2, 3, 13, 31, 32, 33, 1 << BUDDY_MAX_ORDER, CAPACITY};
	for (ksize_t test = 0; test < sizeof(counts) / sizeof(counts[0]);
	     test++) {
		ksize_t count = counts[test];
		struct buddy_arena arena = {.base = BUDDY_PAGE_SIZE,
					    .size = count * BUDDY_PAGE_SIZE};
		pages[count].order = 123;
		memset(covered, 0, sizeof(covered));
		assert(buddy_init(&allocator, &arena, pages, CAPACITY + 1) ==
		       BUDDY_OK);
		assert(allocator.pages == pages &&
		       allocator.page_count == count);
		assert(allocator.arena.base == arena.base &&
		       allocator.arena.size == arena.size);
		assert(pages[count].order ==
		       123); /* Spare capacity is untouched. */

		for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
			struct list_meta *list = &allocator.free_lists[order];
			struct node *previous = NULL;
			ksize_t blocks = 0;
			for (struct node *node = list->head; node != NULL;
			     node = node->next) {
				assert(++blocks <=
				       count); /* Detect a cycle without
						  hanging. */
				struct buddy_page *page = node->data;
				ksize_t index = (ksize_t)(page - pages);
				ksize_t span = (ksize_t)1 << order;
				assert(index < count && span <= count - index);
				assert(index % span == 0);
				assert(page->state == BUDDY_PAGE_FREE &&
				       page->order == order);
				assert(node == &page->free_node &&
				       node->prev == previous);
				for (ksize_t i = index; i < index + span; i++) {
					assert(covered[i] == 0);
					covered[i] = 1;
					if (i != index) {
						assert(pages[i].state ==
						       BUDDY_PAGE_INTERIOR);
						assert(pages[i].order == -1);
						assert(pages[i].free_node
							       .next == NULL);
						assert(pages[i].free_node
							       .prev == NULL);
					}
				}
				ksize_t buddy_index = index ^ span;
				if (order < BUDDY_MAX_ORDER &&
				    buddy_index < count) {
					assert(pages[buddy_index].state !=
						       BUDDY_PAGE_FREE ||
					       pages[buddy_index].order !=
						       order);
				}
				previous = node;
			}
			assert(previous == list->tail && blocks == list->len);
		}
		for (ksize_t i = 0; i < count; i++) {
			assert(covered[i] == 1);
			assert(pages[i].free_node.data == &pages[i]);
			assert(buddy_page_at(&allocator,
					     arena.base +
						     i * BUDDY_PAGE_SIZE) ==
			       &pages[i]);
		}
		assert(buddy_page_at(&allocator,
				     arena.base - BUDDY_PAGE_SIZE) == NULL);
		assert(buddy_page_at(&allocator, arena.base + 1) == NULL);
		assert(buddy_page_at(&allocator, arena.base + arena.size) ==
		       NULL);
	}

	/* A second instance owns separate metadata and list heads. */
	struct buddy_allocator second;
	struct buddy_page second_pages[3];
	struct buddy_arena second_arena = {.base = 0,
					   .size = 3 * BUDDY_PAGE_SIZE};
	assert(buddy_init(&second, &second_arena, second_pages, 3) == BUDDY_OK);
	assert(second.free_lists[1].head == &second_pages[0].free_node);
	assert(second.free_lists[0].head == &second_pages[2].free_node);
	assert(allocator.free_lists[BUDDY_MAX_ORDER].len == 2);
	assert(buddy_page_at(&second, 0) == &second_pages[0]);

	unsigned char allocator_before[sizeof(second)];
	unsigned char pages_before[sizeof(second_pages)];
	memcpy(allocator_before, &second, sizeof(second));
	memcpy(pages_before, second_pages, sizeof(second_pages));
	assert(buddy_init(&second, &second_arena, second_pages, 2) ==
	       BUDDY_METADATA_TOO_SMALL);
	struct buddy_arena bad = {.base = 1, .size = BUDDY_PAGE_SIZE};
	assert(buddy_init(&second, &bad, second_pages, 3) ==
	       BUDDY_INVALID_ARGUMENT);
	bad.base = UPTR_MAX - (BUDDY_PAGE_SIZE - 1);
	assert(buddy_init(&second, &bad, second_pages, 3) ==
	       BUDDY_OUT_OF_RANGE);
	assert(buddy_init(&second, NULL, second_pages, 3) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_init(NULL, &second_arena, second_pages, 3) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(buddy_init(&second, &second_arena, NULL, 3) ==
	       BUDDY_INVALID_ARGUMENT);
	assert(memcmp(allocator_before, &second, sizeof(second)) == 0);
	assert(memcmp(pages_before, second_pages, sizeof(second_pages)) == 0);
	assert(buddy_page_at(NULL, 0) == NULL);
}

int main(void) {
	struct buddy_arena arena = {.base = BUDDY_PAGE_SIZE,
				    .size = (ksize_t)BUDDY_PAGE_SIZE
					    << (BUDDY_MAX_ORDER + 1)};
	round_trips(&arena);
	failures(&arena);
	initialization();
	return 0;
}
