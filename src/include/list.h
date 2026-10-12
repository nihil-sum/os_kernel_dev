#ifndef LIST_H
#define LIST_H

#include "ktypes.h"

/* Nodes and their data are owned by the caller; the list never allocates. */
struct node {
	void *data;
	struct node *next;
	struct node *prev;
};
struct list_meta {
	ksize_t len;
	struct node *head;
	struct node *tail;
};

/* Initialize a new list/node before use. Do not reinitialize linked nodes. */
void list_init(struct list_meta *list);
void node_init(struct node *node, void *data);
int list_empty(const struct list_meta *list);

/* Insert a detached node. A node may belong to only one list at a time. */
void list_push_front(struct list_meta *list, struct node *node);
void list_push_back(struct list_meta *list, struct node *node);

/* The node must belong to this list. Removal clears its links, not its data. */
void list_remove(struct list_meta *list, struct node *node);

/* Return NULL when empty. Pop also removes the returned node. */
struct node *list_front(const struct list_meta *list);
struct node *list_back(const struct list_meta *list);
struct node *list_pop_front(struct list_meta *list);
struct node *list_pop_back(struct list_meta *list);

/* Traverse with node->next from head, or node->prev from tail.
 * Save the next/previous node before removing the current node.
 * Arguments must be non-NULL. Concurrent access needs external locking.
 */

#endif
