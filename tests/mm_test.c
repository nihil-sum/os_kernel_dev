#include "mm.h"
#include <stdio.h>
int main() {
	mm_init();
	
	struct page_t * page1 = page_alloc();

	struct page_t * page2 = page_alloc();

	int result = page1->next == page2;

	printf("%d",result);

	return 0;
}

