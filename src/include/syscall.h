#ifndef SYSCALL_H
#define SYSCALL_H

#include "proc.h"

enum proc_syscall { SYS_FORK = 1, SYS_EXEC = 2, SYS_KILL = 3 };
enum syscall_action { SYSCALL_RETURN, SYSCALL_EXEC, SYSCALL_EXIT };

/* Current must refer to a RUNNING PROC_USER process; kernel callbacks use
 * proc_* APIs directly. Exec's arguments are user VAs:
 * name points to a registered image name; argv is a NULL-terminated RV64
 * array of string pointers (0 means no arguments). No raw user dereference.
 */
int64_t sys_fork(struct proc_table *table);
int sys_exec(struct proc_table *table, uint64_t name, uint64_t argv);
int sys_kill(struct proc_table *table, uint64_t pid);

/* Call only after a trap handler has identified a user ecall. a7 holds the
 * number, a0/a1 the arguments. Advances saved PC by four before fork, so both
 * processes resume past ecall. Errors/ordinary returns go in a0; exec keeps
 * the new PC/SP/argc/argv. EXIT requires leaving the current kernel stack
 * before scheduler completion/reap (or manual deactivate without a scheduler);
 * never return a killed process to user mode.
 * This is the C dispatch boundary, not an installed hardware trap handler.
 */
enum syscall_action syscall_dispatch(struct proc_table *table);
#endif
