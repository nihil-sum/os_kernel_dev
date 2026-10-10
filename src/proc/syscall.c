#include "syscall.h"

static struct proc *current(struct proc_table *table) {
        if (table == NULL || table->current == NULL) return NULL;
        struct proc *p = table->current;
        return p->kind == PROC_USER && p->pstatus == RUNNING &&
               proc_find(table, p->pid) == p ? p : NULL;
}

int64_t sys_fork(struct proc_table *table) {
        struct proc *p = current(table);
        return p == NULL ? PROC_EINVAL : proc_fork(table, p);
}

int sys_exec(struct proc_table *table, uint64_t name, uint64_t argv) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        char image_name[PROC_NAME_MAX];
        int err = proc_copyinstr(p, image_name, name, sizeof(image_name));
        if (err != 0) return err;
        char arguments[PROC_ARG_MAX][PROC_ARG_BYTES];
        const char *kernel_argv[PROC_ARG_MAX];
        size_t argc = 0;
        if (argv != 0) {
                for (;;) {
                        uint64_t string;
                        size_t offset = argc * sizeof(string);
                        if (offset > UINT64_MAX - argv) return PROC_EFAULT;
                        err = proc_copyin(p, &string, argv + offset, sizeof(string));
                        if (err != 0) return err;
                        if (string == 0) break;
                        if (argc == PROC_ARG_MAX) return PROC_E2BIG;
                        err = proc_copyinstr(p, arguments[argc], string, PROC_ARG_BYTES);
                        if (err != 0) return err;
                        kernel_argv[argc] = arguments[argc];
                        argc++;
                }
        }
        return proc_exec(table, p, image_name, argc, kernel_argv);
}

int sys_kill(struct proc_table *table, uint64_t pid) {
        return current(table) == NULL ? PROC_EINVAL : proc_kill(table, pid);
}

enum syscall_action syscall_dispatch(struct proc_table *table) {
        struct proc *p = current(table);
        if (p == NULL) return SYSCALL_EXIT;
        if (p->killed || p->tf.pc > UINT64_MAX - 4) {
                (void)proc_exit(table, p, -1);
                return SYSCALL_EXIT;
        }
        p->tf.x[0] = 0;
        p->tf.pc += 4;
        int64_t result;
        switch (p->tf.x[PROC_A7]) {
        case SYS_FORK:
                result = sys_fork(table);
                break;
        case SYS_EXEC:
                result = sys_exec(table, p->tf.x[PROC_A0], p->tf.x[PROC_A1]);
                if (result == 0) return SYSCALL_EXEC;
                break;
        case SYS_KILL:
                result = sys_kill(table, p->tf.x[PROC_A0]);
                break;
        default:
                result = PROC_ENOSYS;
                break;
        }
        p->tf.x[PROC_A0] = (uint64_t)result;
        if (p->killed) {
                (void)proc_exit(table, p, -1);
                return SYSCALL_EXIT;
        }
        return SYSCALL_RETURN;
}
