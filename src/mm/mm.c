#include "mm.h"
#include <stdint.h>

extern char __kernel_end[];
static struct page_t *free_list;

void mm_init(void) {
	uintptr_t start = ALIGN_UP(__kernel_end);
	uintptr_t end = RAM_END;
	free_list = NULL;
	for (uintptr_t addr = start; addr < end; addr += PAGE_SIZE) {
		struct page_t *page = (struct page_t *)addr;
		page->next = free_list;
		free_list = page;
	}
}

struct page_t *page_alloc(void) {
	if (free_list == NULL) {
		return NULL;
	}
	struct page_t *free_page = free_list;
	free_list = free_list->next;
	zero_page(free_page);
	return free_page;
}

void free_page(void *addr) {
	struct page_t *page = (struct page_t *)addr;
	page->next = free_list;
	free_list = page;
}

void zero_page(struct page_t *page) {
	uint8_t *bptr = (uint8_t *)page;
	for (size_t i = 0; i < PAGE_SIZE; i++) {
		bptr[i] = 0;
	}
}
