# Buddy allocator data model

`buddy_allocator` owns the arena description and one free list per order. The
caller supplies a permanent `buddy_page` array with one entry for each 4 KiB page.
This metadata lives outside the memory being managed, so initialization requires
no allocation and never writes into the arena.

```text
buddy_allocator
  arena                 base address and byte size
  pages[i]              metadata for base + i * 4096
  free_lists[0..11]      free block heads, grouped by order
```

A block of order n contains 2^n pages. Its first page stores its order and state:
`BUDDY_PAGE_FREE` or `BUDDY_PAGE_ALLOCATED`. All remaining pages are
`BUDDY_PAGE_INTERIOR`, with order -1. Only free heads are linked into a free list;
each appears exactly once. `free_node.data` points to its containing page entry.
The list's `len` counts blocks, so its free page count is `len * (1 << order)`.

The small `block` structure is a copyable `{addr, order}` description used by
split/combine address calculations. It has no list links or allocation state.
After calculating a buddy address, use `buddy_page_at()` to find its metadata.
It is eligible for merging only when it is a free head of the same order.

`buddy_init()` partitions the entire arena into maximal aligned free blocks.
For example, 13 pages become blocks of 8, 4, and 1 pages, placed in orders 3, 2,
and 0. Alignment is relative to the arena base. Large arenas can contain several
order-11 blocks.

```c
static struct buddy_allocator allocator;
static struct buddy_page metadata[13];

struct buddy_arena arena = {
    .base = 0x100000,
    .size = 13 * BUDDY_PAGE_SIZE,
};
enum buddy_status status = buddy_init(&allocator, &arena, metadata, 13);
/* Check status == BUDDY_OK before using allocator. */
```

Keep the allocator and page table alive at stable addresses. The entire arena
must be available for allocation; reserved memory and metadata must be excluded.
Callers serialize access. `buddy_alloc()` now finds a suitable free head and
splits it down to the requested order; `buddy_free()` validates an allocated
head and coalesces free same-order buddies. These operations coordinate metadata
and free-list changes, leave allocation outputs untouched on failure, and do
not initialize arena contents. The process module zeroes its own pages.

The redesign replaces `block.head` with `block.addr` and `MAX_ORDER` with
`BUDDY_MAX_ORDER`. The unused `mm_ops`, `free_area`, and unimplemented singleton
`buddy_alloc` declaration have been removed.

Run the tests from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -Isrc/include src/list.c src/buddy_mm.c tests/buddy_mm_test.c -o /tmp/os_learn_buddy_test
/tmp/os_learn_buddy_test
```

## QEMU boot check

`make qemu` cross-compiles a small freestanding RISC-V image and starts QEMU
`virt` with 128 MiB of RAM and no firmware. It needs
`riscv64-linux-gnu-gcc` and `qemu-system-riscv64`. The linker places the image
at `0x80000000`; `boot.S` sets a stack, clears BSS, and calls `kmain`.

The check reserves 256 pages (1 MiB) just above `__kernel_end`, initializes
the buddy metadata in BSS, then checks one split, buddy lookup, and combine.
It prints `BUDDY OK` on the serial console and exits QEMU with status 0 on
success. It also runs the process lifecycle check described in
[`proc/proc.md`](proc/proc.md), printing `PROC OK` before exiting. It prints
`BUDDY FAIL` or `PROC FAIL` and exits nonzero on failure. The memory map
constants live in `src/include/qemu_virt.h`; the buddy size and alignment macros
live in `src/include/buddy_mm.h`. The QEMU smoke-test entry point lives in
`tests/qemu_buddy.c`.

The older `mm.c` page allocator uses the same RAM end and linker-provided
kernel end. It should not manage pages simultaneously with the buddy allocator
unless their arenas are disjoint.
