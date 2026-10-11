#include "list.h"

void list_init(struct list_meta *list) {
  list->len = 0;
  list->head = NULL;
  list->tail = NULL;
}

void node_init(struct node *node, void *data) {
  node->data = data;
  node->next = NULL;
  node->prev = NULL;
}

int list_empty(const struct list_meta *list) { return list->head == NULL; }

void list_push_front(struct list_meta *list, struct node *node) {
  node->prev = NULL;
  node->next = list->head;
  if (list->head != NULL) {
    list->head->prev = node;
  } else {
    list->tail = node;
  }
  list->head = node;
  list->len++;
}

void list_push_back(struct list_meta *list, struct node *node) {
  node->next = NULL;
  node->prev = list->tail;
  if (list->tail != NULL) {
    list->tail->next = node;
  } else {
    list->head = node;
  }
  list->tail = node;
  list->len++;
}

void list_remove(struct list_meta *list, struct node *node) {
  if (node->prev != NULL) {
    node->prev->next = node->next;
  } else {
    list->head = node->next;
  }
  if (node->next != NULL) {
    node->next->prev = node->prev;
  } else {
    list->tail = node->prev;
  }
  list->len--;
  node->next = NULL;
  node->prev = NULL;
}

struct node *list_front(const struct list_meta *list) { return list->head; }

struct node *list_back(const struct list_meta *list) { return list->tail; }

struct node *list_pop_front(struct list_meta *list) {
  struct node *node = list->head;
  if (node != NULL) {
    list_remove(list, node);
  }
  return node;
}

struct node *list_pop_back(struct list_meta *list) {
  struct node *node = list->tail;
  if (node != NULL) {
    list_remove(list, node);
  }
  return node;
}
