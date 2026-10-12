#include "list.h"
#include <assert.h>

/* Check ordering and reciprocal links independently in both directions. */
static void check(const struct list_meta *list, struct node **expected,
		  ksize_t count) {
	assert(list->len == count);
	assert(list_empty(list) == (count == 0));
	assert(list_front(list) == (count ? expected[0] : NULL));
	assert(list_back(list) == (count ? expected[count - 1] : NULL));
	struct node *node = list->head;
	for (ksize_t i = 0; i < count; i++) {
		assert(node == expected[i]);
		assert(node->prev == (i ? expected[i - 1] : NULL));
		node = node->next;
	}
	assert(node == NULL);
	node = list->tail;
	for (ksize_t i = count; i > 0; i--) {
		assert(node == expected[i - 1]);
		assert(node->next == (i < count ? expected[i] : NULL));
		node = node->prev;
	}
	assert(node == NULL);
}

int main(void) {
	struct list_meta first, second;
	struct node a, b, c;
	int data = 42;
	list_init(&first);
	list_init(&second);
	node_init(&a, &data);
	node_init(&b, NULL);
	node_init(&c, NULL);
	check(&first, NULL, 0);
	assert(list_pop_front(&first) == NULL);
	assert(list_pop_back(&first) == NULL);

	list_push_front(&first, &b);
	check(&first, (struct node *[]){&b}, 1);
	list_push_front(&first, &a);
	list_push_back(&first, &c);
	check(&first, (struct node *[]){&a, &b, &c}, 3);
	list_remove(&first, &b);
	check(&first, (struct node *[]){&a, &c}, 2);
	assert(b.next == NULL && b.prev == NULL);

	list_push_back(&second, &b);
	check(&second, (struct node *[]){&b}, 1);
	assert(list_pop_front(&first) == &a);
	assert(a.data == &data && a.next == NULL && a.prev == NULL);
	check(&first, (struct node *[]){&c}, 1);
	assert(list_pop_back(&first) == &c);
	check(&first, NULL, 0);
	assert(list_pop_back(&second) == &b);
	check(&second, NULL, 0);

	list_push_back(&first, &a);
	list_push_back(&first, &b);
	list_push_front(&first, &c);
	check(&first, (struct node *[]){&c, &a, &b}, 3);
	assert(list_pop_back(&first) == &b);
	check(&first, (struct node *[]){&c, &a}, 2);
	assert(list_pop_front(&first) == &c);
	check(&first, (struct node *[]){&a}, 1);
	assert(list_pop_front(&first) == &a);
	check(&first, NULL, 0);
	return 0;
}
