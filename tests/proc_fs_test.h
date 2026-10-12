#ifndef PROC_FS_TEST_H
#define PROC_FS_TEST_H

#include "buddy_mm.h"

/* Freestanding QEMU checks: zero on success, otherwise the failing line. */
int proc_fs_test(struct buddy_allocator *allocator);

#endif
