#ifndef MM_H
#define MM_H

#include "buddy_mm.h"
#include "qemu_virt.h"

#define PAGE_SIZE BUDDY_PAGE_SIZE
#define ALIGN_UP(value) BUDDY_ALIGN_UP(value)
#define RAM_END QEMU_RAM_END

struct page_t {
	struct page_t *next;
};
void mm_init(void);
struct page_t *page_alloc(void);
void free_page(void *addr);
void zero_page(struct page_t *page);

#endif
