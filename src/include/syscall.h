#ifndef SYSCALL_H
#define SYSCALL_H

#include "proc.h"

enum proc_syscall {
        SYS_FORK = 1, SYS_EXEC = 2, SYS_KILL = 3,
        SYS_OPEN = 4, SYS_CLOSE = 5, SYS_READ = 6, SYS_WRITE = 7,
        SYS_DUP = 8, SYS_CHDIR = 9, SYS_MKDIR = 10, SYS_UNLINK = 11,
        SYS_EXIT = 12
};
enum syscall_action { SYSCALL_RETURN, SYSCALL_EXEC, SYSCALL_EXIT };

/* Current must refer to a RUNNING PROC_USER process; kernel callbacks use
 * proc_* APIs directly. Exec's arguments are user VAs:
 * name points to a filesystem path (or a registered image name on a table
 * without a filesystem); argv is a NULL-terminated RV64 array of string
 * pointers (0 means no arguments). Paths and I/O buffers are user VAs.
 * Each I/O syscall checks the entire buffer before changing file offsets or
 * contents. All calls require the table's normal caller serialization.
 */
i64_t sys_fork(struct proc_table *table);
int sys_exec(struct proc_table *table, u64_t name, u64_t argv);
int sys_kill(struct proc_table *table, u64_t pid);
int sys_open(struct proc_table *table, u64_t path, u64_t flags);
int sys_close(struct proc_table *table, u64_t fd);
i64_t sys_read(struct proc_table *table, u64_t fd, u64_t dst, u64_t count);
i64_t sys_write(struct proc_table *table, u64_t fd, u64_t src, u64_t count);
int sys_dup(struct proc_table *table, u64_t fd);
int sys_chdir(struct proc_table *table, u64_t path);
int sys_mkdir(struct proc_table *table, u64_t path);
int sys_unlink(struct proc_table *table, u64_t path);
int sys_exit(struct proc_table *table, u64_t status);

/* Call only after a trap handler has identified a user ecall. a7 holds the
 * number, a0/a1/a2 the arguments. Advances saved PC by four before fork, so both
 * processes resume past ecall. Errors/ordinary returns go in a0; exec keeps
 * the new PC/SP/argc/argv. EXIT requires leaving the current kernel stack
 * before scheduler completion/reap (or manual deactivate without a scheduler);
 * never return a killed process to user mode.
 * This is the C dispatch boundary, not an installed hardware trap handler.
 */
enum syscall_action syscall_dispatch(struct proc_table *table);
#endif
