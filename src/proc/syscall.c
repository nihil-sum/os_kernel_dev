#include "syscall.h"
#include "proc_fs.h"
#include "kalloc.h"

static struct proc *current(struct proc_table *table) {
        if (table == NULL || table->current == NULL) return NULL;
        struct proc *p = table->current;
        return p->kind == PROC_USER && p->pstatus == RUNNING &&
               proc_find(table, p->pid) == p ? p : NULL;
}

i64_t sys_fork(struct proc_table *table) {
        struct proc *p = current(table);
        return p == NULL ? PROC_EINVAL : proc_fork(table, p);
}

struct exec_arguments {
        char strings[PROC_ARG_MAX][PROC_ARG_BYTES];
        const char *pointers[PROC_ARG_MAX];
        ksize_t count;
};

static int copy_arguments(struct proc *p, u64_t argv,
                          struct exec_arguments *args) {
        args->count = 0;
        if (argv != 0) {
                for (;;) {
                        u64_t string;
                        ksize_t offset = args->count * sizeof(string);
                        if (offset > U64_MAX - argv) return PROC_EFAULT;
                        int err = proc_copyin(p, &string, argv + offset, sizeof(string));
                        if (err != 0) return err;
                        if (string == 0) break;
                        if (args->count == PROC_ARG_MAX) return PROC_E2BIG;
                        err = proc_copyinstr(p, args->strings[args->count], string,
                                            PROC_ARG_BYTES);
                        if (err != 0) return err;
                        args->pointers[args->count] = args->strings[args->count];
                        args->count++;
                }
        }
        return 0;
}

/* Registry-only tables also work before kalloc initialization. Keep their
 * original bounded stack staging separate from filesystem exec staging.
 */
static int exec_registry(struct proc_table *table, struct proc *p,
                         u64_t name, u64_t argv) {
        char image_name[PROC_NAME_MAX];
        int err = proc_copyinstr(p, image_name, name, sizeof(image_name));
        if (err != 0) return err;
        struct exec_arguments args;
        err = copy_arguments(p, argv, &args);
        if (err != 0) return err;
        return proc_exec(table, p, image_name, args.count, args.pointers);
}

int sys_exec(struct proc_table *table, u64_t name, u64_t argv) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (table->filesystem == NULL) return exec_registry(table, p, name, argv);
        struct exec_staging {
                char path[FS_PATH_MAX + 1];
                struct exec_arguments args;
        };
        struct exec_staging *stage = kalloc(sizeof(*stage));
        if (stage == NULL) return PROC_ENOMEM;
        int err = proc_copyinstr(p, stage->path, name, sizeof(stage->path));
        if (err == 0) err = copy_arguments(p, argv, &stage->args);
        if (err == 0)
                err = proc_exec(table, p, stage->path, stage->args.count,
                                stage->args.pointers);
        (void)kfree(stage);
        return err;
}

int sys_kill(struct proc_table *table, u64_t pid) {
        return current(table) == NULL ? PROC_EINVAL : proc_kill(table, pid);
}

int sys_open(struct proc_table *table, u64_t path, u64_t flags) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (flags > U32_MAX) return PROC_EINVAL;
        char kernel_path[FS_PATH_MAX + 1];
        int err = proc_copyinstr(p, kernel_path, path, sizeof(kernel_path));
        return err != 0 ? err : proc_open(table, p, kernel_path, (unsigned int)flags);
}

int sys_close(struct proc_table *table, u64_t fd) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (fd >= PROC_FD_MAX) return PROC_EBADF;
        return proc_close(table, p, (int)fd);
}

i64_t sys_read(struct proc_table *table, u64_t fd, u64_t dst, u64_t count) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (fd >= PROC_FD_MAX) return PROC_EBADF;
        int err = proc_user_range(p, dst, count, PROC_WRITE);
        if (err != 0) return err;
        /* Check descriptor and mode even for a zero-byte request. */
        i64_t result = proc_read(table, p, (int)fd, NULL, 0);
        if (result < 0 || count == 0) return result;
        unsigned char buffer[FS_BLOCK_SIZE];
        ksize_t done = 0;
        while (done < count) {
                ksize_t chunk = count - done;
                if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
                result = proc_read(table, p, (int)fd, buffer, chunk);
                if (result < 0) return done != 0 ? (i64_t)done : result;
                if (result == 0) break;
                /* The complete range was validated and no call here yields
                 * or changes mappings, so this copy cannot encounter a new
                 * user-memory fault under the serialization contract.
                 */
                err = proc_copyout(p, dst + done, buffer, (ksize_t)result);
                if (err != 0) return done != 0 ? (i64_t)done : err;
                done += (ksize_t)result;
                if ((ksize_t)result < chunk) break;
        }
        return (i64_t)done;
}

i64_t sys_write(struct proc_table *table, u64_t fd, u64_t src, u64_t count) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (fd >= PROC_FD_MAX) return PROC_EBADF;
        if (count > FS_MAX_FILE_SIZE) return PROC_EFBIG;
        int err = proc_user_range(p, src, count, PROC_READ);
        if (err != 0) return err;
        i64_t result = proc_write(table, p, (int)fd, NULL, 0);
        if (result < 0 || count == 0) return result;
        /* Preserve inode_write's all-or-nothing allocation semantics: gather
         * the entire user buffer, then issue exactly one filesystem write.
         */
        void *buffer = kalloc(count);
        if (buffer == NULL) return PROC_ENOMEM;
        err = proc_copyin(p, buffer, src, count);
        result = err != 0 ? err : proc_write(table, p, (int)fd, buffer, count);
        (void)kfree(buffer);
        return result;
}

int sys_dup(struct proc_table *table, u64_t fd) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        if (fd >= PROC_FD_MAX) return PROC_EBADF;
        return proc_dup(table, p, (int)fd);
}

int sys_chdir(struct proc_table *table, u64_t path) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        char kernel_path[FS_PATH_MAX + 1];
        int err = proc_copyinstr(p, kernel_path, path, sizeof(kernel_path));
        return err != 0 ? err : proc_chdir(table, p, kernel_path);
}

int sys_mkdir(struct proc_table *table, u64_t path) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        char kernel_path[FS_PATH_MAX + 1];
        int err = proc_copyinstr(p, kernel_path, path, sizeof(kernel_path));
        return err != 0 ? err : proc_mkdir(table, p, kernel_path);
}

int sys_unlink(struct proc_table *table, u64_t path) {
        struct proc *p = current(table);
        if (p == NULL) return PROC_EINVAL;
        char kernel_path[FS_PATH_MAX + 1];
        int err = proc_copyinstr(p, kernel_path, path, sizeof(kernel_path));
        return err != 0 ? err : proc_unlink(table, p, kernel_path);
}

int sys_exit(struct proc_table *table, u64_t status) {
        struct proc *p = current(table);
        return p == NULL ? PROC_EINVAL : proc_exit(table, p, (i32_t)status);
}

enum syscall_action syscall_dispatch(struct proc_table *table) {
        struct proc *p = current(table);
        if (p == NULL) return SYSCALL_EXIT;
        if (p->killed || p->tf.pc > U64_MAX - 4) {
                (void)proc_exit(table, p, -1);
                return SYSCALL_EXIT;
        }
        p->tf.x[0] = 0;
        p->tf.pc += 4;
        i64_t result;
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
        case SYS_OPEN:
                result = sys_open(table, p->tf.x[PROC_A0], p->tf.x[PROC_A1]);
                break;
        case SYS_CLOSE:
                result = sys_close(table, p->tf.x[PROC_A0]);
                break;
        case SYS_READ:
                result = sys_read(table, p->tf.x[PROC_A0], p->tf.x[PROC_A1],
                                  p->tf.x[PROC_A2]);
                break;
        case SYS_WRITE:
                result = sys_write(table, p->tf.x[PROC_A0], p->tf.x[PROC_A1],
                                   p->tf.x[PROC_A2]);
                break;
        case SYS_DUP:
                result = sys_dup(table, p->tf.x[PROC_A0]);
                break;
        case SYS_CHDIR:
                result = sys_chdir(table, p->tf.x[PROC_A0]);
                break;
        case SYS_MKDIR:
                result = sys_mkdir(table, p->tf.x[PROC_A0]);
                break;
        case SYS_UNLINK:
                result = sys_unlink(table, p->tf.x[PROC_A0]);
                break;
        case SYS_EXIT:
                result = sys_exit(table, p->tf.x[PROC_A0]);
                if (result == 0) return SYSCALL_EXIT;
                break;
        default:
                result = PROC_ENOSYS;
                break;
        }
        p->tf.x[PROC_A0] = (u64_t)result;
        if (p->killed) {
                (void)proc_exit(table, p, -1);
                return SYSCALL_EXIT;
        }
        return SYSCALL_RETURN;
}
