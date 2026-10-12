#ifndef KALLOC_H
#define KALLOC_H

#include "buddy_mm.h"
#include "ktypes.h"

#define KALLOC_MIN_SIZE 16u
#define KALLOC_SLAB_MAX_SIZE 2048u

enum kalloc_status {
	KALLOC_OK = 0,
	KALLOC_INVALID_ARGUMENT,
	KALLOC_NOT_ALLOCATED,
	KALLOC_BUSY
};

/* Bind the kernel heap to an initialized, directly accessible buddy arena.
 * Initialization allocates no pages. Allocator and buddy metadata must remain
 * at stable addresses. Rebinding is allowed only with no live allocations;
 * failure leaves the heap unchanged. Do not reinitialize the backing buddy
 * allocator or free heap-owned blocks through buddy_free().
 * All heap and backing buddy operations require caller-provided serialization.
 */
enum kalloc_status kalloc_init(struct buddy_allocator *allocator);

/* Return at least size bytes, aligned for kmax_align_t, with unspecified
 * contents. Sizes <= 2048 use power-of-two slab caches (16..2048 bytes);
 * larger sizes use a buddy block with an internal header. Return NULL before
 * initialization, for size 0, oversized requests, or memory exhaustion.
 */
void *kalloc(ksize_t size);

/* Free an exact live pointer returned by kalloc(); NULL is a successful no-op.
 * Foreign/interior pointers and double frees are rejected without touching
 * their contents. A stale pointer cannot be distinguished from a new object
 * subsequently allocated at the same address. Empty slabs are returned to
 * buddy immediately. The size need not be supplied by the caller.
 */
enum kalloc_status kfree(void *ptr);

#endif
