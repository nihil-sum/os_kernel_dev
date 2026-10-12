#ifndef SHELL_H
#define SHELL_H

#include "mlfq.h"

/* Interactive kernel stage-test shell. Runs on the boot stack while MLFQ
 * jobs use their own stacks. Call once with initialized, idle objects that
 * stay alive until shutdown. The table must have a mounted filesystem; shell
 * commands retain their own cwd reference and release it at shutdown.
 */
_Noreturn void shell_main_loop(struct buddy_allocator *allocator,
                               struct proc_table *table, struct mlfq *scheduler);
#endif
