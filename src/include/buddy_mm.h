#ifndef BUDDY_MM_H
#define BUDDY_MM_H

#include "list.h"
#include "ktypes.h"

#define BUDDY_PAGE_SIZE 4096u
/* Inclusive: supported orders are 0 through BUDDY_MAX_ORDER. */
#define BUDDY_MAX_ORDER 11
/* The arena base and size passed to buddy_init() must be page-aligned. */
#define BUDDY_ALIGN_UP(value)                                                  \
	(((uptr_t)(value) + BUDDY_PAGE_SIZE - 1) &                          \
	 ~(uptr_t)(BUDDY_PAGE_SIZE - 1))
#define BUDDY_BLOCK_BYTES(order) ((ksize_t)BUDDY_PAGE_SIZE << (order))

/* A copyable description, with no ownership or list links.
 * A block spans BUDDY_PAGE_SIZE * 2^order bytes starting at addr.
 */
struct block {
	uptr_t addr;
	int order;
};

/* base must be page-aligned; size must be a nonzero multiple of page size.
 * Block alignment and buddy calculations are relative to base. The arena
 * need not have a power-of-two size; blocks must fit entirely inside it.
 */
struct buddy_arena {
	uptr_t base;
	ksize_t size;
};

enum buddy_page_state {
	BUDDY_PAGE_INTERIOR, /* Part of a larger block; order is -1. */
	BUDDY_PAGE_FREE,     /* Block head linked in free_lists[order]. */
	BUDDY_PAGE_ALLOCATED /* Block head; free_node is detached. */
};

/* One permanent descriptor per minimum-size page. Only block heads have a
 * valid order. Interior and allocated pages must have detached list nodes.
 * free_node.data points back to this descriptor, avoiding container casts.
 */
struct buddy_page {
	enum buddy_page_state state;
	int order;
	struct node free_node;
};

/* pages[i] describes arena.base + i * BUDDY_PAGE_SIZE.
 * Each free block appears exactly once in its order's list. List len is the
 * number of blocks, not pages. No separate counter needs to be kept in sync.
 */
struct buddy_allocator {
	struct buddy_arena arena;
	struct buddy_page *pages;
	ksize_t page_count;
	struct list_meta free_lists[BUDDY_MAX_ORDER + 1];
};

enum buddy_status {
	BUDDY_OK = 0,
	BUDDY_INVALID_ARGUMENT,
	BUDDY_INVALID_ORDER,
	BUDDY_UNALIGNED,
	BUDDY_OUT_OF_RANGE,
	BUDDY_NOT_BUDDIES,
	BUDDY_METADATA_TOO_SMALL,
	BUDDY_NO_MEMORY,
	BUDDY_NOT_ALLOCATED
};

/* Initialize an entirely free arena as maximal aligned blocks.
 * page_capacity counts descriptors, and must be >= arena.size / page size.
 * The caller supplies metadata storage outside the managed memory, disjoint
 * from allocator, and keeps both alive at stable addresses. Do not copy a live
 * allocator or its page table. Reinitialize only after all users are finished.
 * Failure leaves allocator and pages unchanged. No arena bytes are accessed.
 */
enum buddy_status buddy_init(struct buddy_allocator *allocator,
			     const struct buddy_arena *arena,
			     struct buddy_page *pages, ksize_t page_capacity);

/* Look up an exact page address in an initialized allocator. Returns NULL for
 * unaligned/out-of-range addresses. Inspect state before treating it as a head.
 */
struct buddy_page *buddy_page_at(const struct buddy_allocator *allocator,
				 uptr_t addr);

/* Allocate a block, splitting larger free blocks as needed. Output is
 * unchanged on failure. Memory contents are not initialized. Free requires
 * the exact allocated head and order; same-order free buddies coalesce.
 * The arena must be directly accessible if callers dereference block.addr.
 * As with other allocator operations, callers serialize access.
 */
enum buddy_status buddy_alloc(struct buddy_allocator *allocator, int order,
                             struct block *block);
enum buddy_status buddy_free(struct buddy_allocator *allocator,
                            const struct block *block);

/* Return the same-order buddy, if it fits in the arena.
 * Success describes its address only, not whether it is free or allocated.
 */
enum buddy_status buddy_get_buddy(const struct buddy_arena *arena,
				  const struct block *block,
				  struct block *buddy);

/* Split one level. left is the lower-address half; right is the upper half.
 * Order 0 cannot split. left and right must be distinct output objects.
 */
enum buddy_status buddy_split(const struct buddy_arena *arena,
			      const struct block *parent, struct block *left,
			      struct block *right);

/* Combine one level. Inputs must be equal-order buddies, in either order.
 * Two adjacent blocks are not necessarily buddies. BUDDY_MAX_ORDER cannot
 * combine.
 */
enum buddy_status buddy_combine(const struct buddy_arena *arena,
				const struct block *first,
				const struct block *second,
				struct block *parent);

/* All three operations leave outputs unchanged on failure. Outputs may alias
 * input block objects. They allocate no memory and do not touch page contents,
 * track allocation state, or change free lists. Before splitting/combining,
 * the caller must own the blocks, verify they are free, and detach their list
 * nodes. After success, retire the input descriptors and enqueue the outputs
 * as appropriate. Synchronization belongs to the caller.
 */

#endif
