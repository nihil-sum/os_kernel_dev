#ifndef INODE_TEST_H
#define INODE_TEST_H
#include "buddy_mm.h"
/* Freestanding checks: 0 on success, otherwise the failing source line. */
int inode_test(struct buddy_allocator *allocator);
#endif
