# Kernel slab allocation

`kalloc.h` provides a single kernel heap backed by an initialized buddy
allocator. The arena must be directly accessible, and its memory must exclude
the kernel, buddy descriptors, and other reserved storage.

```c
#include "kalloc.h"

/* After successful buddy_init(&allocator, ...): */
if (kalloc_init(&allocator) != KALLOC_OK)
    return -1;

struct example { unsigned int id; char name[24]; };
struct example *object = kalloc(sizeof(*object));
if (object == NULL)
    return -1;
object->id = 42;
/* Initialize the remaining fields before use. */
enum kalloc_status status = kfree(object);
/* Check status == KALLOC_OK. */
```

Requests up to 2048 bytes select the smallest fitting cache from sizes
16, 32, 64, 128, 256, 512, 1024, and 2048. Each slab occupies one order-0 buddy
page. A header and allocation bitmap precede the object area; the remaining
space is divided into slots of the cache's object size. Free slots hold links
to the next free slot. Objects are aligned for `kmax_align_t`; their contents
are unspecified, so callers initialize their fields.

Each cache tracks partial slabs and full slabs separately. Allocation takes
a free slot from a partial slab, or obtains a new page if none has room. A
filled slab moves to the full list. Freeing a slot in a full slab moves it back
to the partial list. Freeing the last live slot removes the slab and releases
the page to buddy immediately, allowing coalescing and reuse by other caches
or page consumers. There is no retained empty-slab cache, constructor API,
object coloring, or built-in synchronization in this basic implementation.

Requests above 2048 bytes use the smallest buddy order that holds both the
requested payload and an aligned internal header. The maximum payload is
`BUDDY_BLOCK_BYTES(BUDDY_MAX_ORDER)` minus that header, and allocation still
depends on available contiguous memory. Zero sizes, uninitialized heaps,
oversized requests, and exhaustion return `NULL` without changing live objects.

`kfree(NULL)` succeeds. Other frees must use an exact live returned pointer;
the caller does not supply its size. The allocator searches its owned slab and
large-block lists before inspecting their metadata, so foreign pointers and
interior pointers are rejected without dereferencing them. The bitmap rejects
double frees even while other objects keep the slab alive. A stale pointer
that aliases a subsequently allocated object cannot be detected. Finding a
pointer's owning slab or large block takes linear time in the number of live
slabs/large blocks; taking or returning a slot itself takes constant time.

Keep the buddy allocator and its descriptors alive at stable addresses, and
serialize all heap operations with every other user of that buddy allocator.
Direct buddy consumers can share the arena, but must never free heap-owned
blocks or reinitialize buddy while the heap is in use. `kalloc_init` allocates
no pages and rejects rebinding with `KALLOC_BUSY` while any heap object is live.
Once all objects are freed, rebinding to another initialized allocator is safe.

`make test` exercises slab reuse, all size classes, alignment, invalid frees,
memory exhaustion, large blocks, mixed allocation patterns, and buddy recovery.
`make qemu` also checks this API on RISC-V and prints `KALLOC OK`. The interactive
shell initializes the heap during boot for use by kernel code.
