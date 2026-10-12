#include "proc_fs.h"
#include "fs.h"
#include "kalloc.h"

/* Every occupied fd owns one reference to an open description. The embedded
 * file owns exactly one inode reference, irrespective of dup/fork aliases.
 */
struct proc_file {
        struct file file;
        u32_t refs;
};

int proc_fs_error(int error) {
        switch (error) {
        case FS_OK: return 0;
        case FS_EINVAL: return PROC_EINVAL;
        case FS_ENOMEM: return PROC_ENOMEM;
        case FS_ENOENT: return PROC_ENOENT;
        case FS_EEXIST: return PROC_EEXIST;
        case FS_ENOTDIR: return PROC_ENOTDIR;
        case FS_EISDIR: return PROC_EISDIR;
        case FS_ENOTEMPTY: return PROC_ENOTEMPTY;
        case FS_EBUSY: return PROC_EBUSY;
        case FS_ENAMETOOLONG: return PROC_E2BIG;
        case FS_EFBIG: return PROC_EFBIG;
        case FS_EXDEV: return PROC_EXDEV;
        case FS_EPERM: return PROC_EPERM;
        case FS_EOVERFLOW: return PROC_EOVERFLOW;
        default: return PROC_EINVAL;
        }
}

static int process_check(const struct proc_table *table, const struct proc *proc) {
        if (table == NULL || table->allocator == NULL || proc == NULL)
                return PROC_EINVAL;
        int found = 0;
        for (ksize_t i = 0; i < PROC_MAX; i++)
                if (&table->entries[i] == proc) { found = 1; break; }
        if (!found || proc->killed ||
            (proc->pstatus != USED && proc->pstatus != RUNNABLE &&
             proc->pstatus != RUNNING && proc->pstatus != SLEEPING))
                return PROC_EINVAL;
        if (table->filesystem == NULL || table->fs_root == NULL)
                return PROC_ENOSYS;
        if (proc->cwd == NULL || proc->cwd->fs != table->filesystem)
                return PROC_EINVAL;
        return 0;
}

static int path_length(const char *path, ksize_t *length) {
        if (path == NULL || path[0] == '\0') return PROC_EINVAL;
        ksize_t n = 0;
        while (n <= FS_PATH_MAX && path[n] != '\0') n++;
        if (n > FS_PATH_MAX) return PROC_E2BIG;
        *length = n;
        return 0;
}

/* Resolve the parent without losing '/' after the final component. Mutation
 * of a root-only path is forbidden; fs_lookup handles root for read/chdir.
 */
static int path_parent(struct proc_table *table, struct proc *proc,
                       const char *path, struct inode **parent,
                       char name[FS_NAME_MAX + 1], int *trailing_slash) {
        ksize_t length;
        int err = path_length(path, &length);
        if (err != 0) return err;
        ksize_t end = length;
        while (end != 0 && path[end - 1] == '/') end--;
        if (end == 0) return PROC_EPERM;
        ksize_t begin = end;
        while (begin != 0 && path[begin - 1] != '/') begin--;
        ksize_t name_size = end - begin;
        if (name_size > FS_NAME_MAX) return PROC_E2BIG;
        for (ksize_t i = 0; i < name_size; i++) name[i] = path[begin + i];
        name[name_size] = '\0';
        *trailing_slash = end != length;
        if (begin == 0) {
                err = inode_ref(proc->cwd);
                if (err == FS_OK) *parent = proc->cwd;
                return proc_fs_error(err);
        }
        char prefix[FS_PATH_MAX + 1];
        for (ksize_t i = 0; i < begin; i++) prefix[i] = path[i];
        prefix[begin] = '\0';
        return proc_fs_error(fs_lookup(table->filesystem, proc->cwd, prefix, parent));
}

static int unused_fd(const struct proc *proc) {
        for (ksize_t i = 0; i < PROC_FD_MAX; i++)
                if (proc->files[i] == NULL) return (int)i;
        return PROC_EMFILE;
}

static struct proc_file *get_file(const struct proc *proc, int fd) {
        if (fd < 0 || (unsigned int)fd >= PROC_FD_MAX) return NULL;
        return proc->files[fd];
}

static void description_put(struct proc_file *description) {
        if (description == NULL) return;
        if (--description->refs != 0) return;
        (void)file_close(&description->file);
        (void)kfree(description);
}

int proc_fs_start(struct proc_table *table, struct proc *proc) {
        if (table == NULL || proc == NULL) return PROC_EINVAL;
        if (proc->cwd != NULL) return PROC_EBUSY;
        if (table->filesystem == NULL) return 0;
        if (table->fs_root == NULL || table->fs_root->fs != table->filesystem)
                return PROC_EINVAL;
        int err = inode_ref(table->fs_root);
        if (err == FS_OK) proc->cwd = table->fs_root;
        return proc_fs_error(err);
}

int proc_fs_fork(struct proc *child, const struct proc *parent) {
        if (child == NULL || parent == NULL || child == parent) return PROC_EINVAL;
        for (ksize_t i = 0; i < PROC_FD_MAX; i++)
                if (child->files[i] != NULL) return PROC_EBUSY;
        struct inode *cwd = parent->cwd;
        if (cwd != NULL) {
                int err = inode_ref(cwd);
                if (err != FS_OK) return proc_fs_error(err);
        }
        for (ksize_t i = 0; i < PROC_FD_MAX; i++) {
                struct proc_file *description = parent->files[i];
                if (description == NULL) continue;
                if (description->refs == U32_MAX) {
                        for (ksize_t j = 0; j < i; j++) {
                                description_put(child->files[j]);
                                child->files[j] = NULL;
                        }
                        if (cwd != NULL) (void)inode_put(cwd);
                        return PROC_EOVERFLOW;
                }
                description->refs++;
                child->files[i] = description;
        }
        struct inode *old_cwd = child->cwd;
        child->cwd = cwd;
        if (old_cwd != NULL) (void)inode_put(old_cwd);
        return 0;
}

void proc_fs_release(struct proc *proc) {
        if (proc == NULL) return;
        for (ksize_t i = 0; i < PROC_FD_MAX; i++) {
                description_put(proc->files[i]);
                proc->files[i] = NULL;
        }
        struct inode *cwd = proc->cwd;
        proc->cwd = NULL;
        if (cwd != NULL) (void)inode_put(cwd);
}

int proc_open(struct proc_table *table, struct proc *proc, const char *path,
              unsigned int flags) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        unsigned int mode = flags & (PROC_O_READ | PROC_O_WRITE);
        if (mode == 0 ||
            (flags & ~(PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE |
                       PROC_O_EXCL | PROC_O_TRUNC)) != 0 ||
            ((flags & PROC_O_EXCL) != 0 && (flags & PROC_O_CREATE) == 0) ||
            ((flags & PROC_O_TRUNC) != 0 && (flags & PROC_O_WRITE) == 0))
                return PROC_EINVAL;
        ksize_t length;
        err = path_length(path, &length);
        if (err != 0) return err;
        (void)length;
        int fd = unused_fd(proc);
        if (fd < 0) return fd;
        struct proc_file *description = kalloc(sizeof(*description));
        if (description == NULL) return PROC_ENOMEM;
        *description = (struct proc_file){.refs = 1};
        struct inode *inode = NULL;
        struct inode *parent = NULL;
        char name[FS_NAME_MAX + 1];
        int create = 0;
        err = fs_lookup(table->filesystem, proc->cwd, path, &inode);
        if (err == FS_OK) {
                if ((flags & PROC_O_EXCL) != 0) { err = PROC_EEXIST; goto fail; }
        } else if (err == FS_ENOENT && (flags & PROC_O_CREATE) != 0) {
                int trailing_slash;
                err = path_parent(table, proc, path, &parent, name, &trailing_slash);
                if (err != 0) goto fail;
                if (trailing_slash) { err = PROC_ENOTDIR; goto fail; }
                /* Reserve the open handle before publishing its directory name.
                 * Thus failure cannot leave a newly created file behind.
                 */
                err = inode_alloc(table->filesystem, FILE_NODE, &inode);
                if (err != FS_OK) { err = proc_fs_error(err); goto fail; }
                create = 1;
        } else {
                err = proc_fs_error(err);
                goto fail;
        }
        unsigned int file_mode = 0;
        if ((mode & PROC_O_READ) != 0) file_mode |= FILE_READ;
        if ((mode & PROC_O_WRITE) != 0) file_mode |= FILE_WRITE;
        err = file_open(&description->file, inode, file_mode);
        if (err != FS_OK) { err = proc_fs_error(err); goto fail; }
        if (create) err = dir_link(parent, name, inode);
        else if ((flags & PROC_O_TRUNC) != 0) err = inode_truncate(inode, 0);
        if (err != FS_OK) { err = proc_fs_error(err); goto fail; }
        if (parent != NULL) (void)inode_put(parent);
        (void)inode_put(inode);
        proc->files[fd] = description;
        return fd;
fail:
        if (parent != NULL) (void)inode_put(parent);
        if (description->file.inode != NULL) (void)file_close(&description->file);
        if (inode != NULL) (void)inode_put(inode);
        (void)kfree(description);
        return err;
}

int proc_close(struct proc_table *table, struct proc *proc, int fd) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct proc_file *description = get_file(proc, fd);
        if (description == NULL) return PROC_EBADF;
        proc->files[fd] = NULL;
        description_put(description);
        return 0;
}

int proc_dup(struct proc_table *table, struct proc *proc, int fd) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct proc_file *description = get_file(proc, fd);
        if (description == NULL) return PROC_EBADF;
        int new_fd = unused_fd(proc);
        if (new_fd < 0) return new_fd;
        if (description->refs == U32_MAX) return PROC_EOVERFLOW;
        description->refs++;
        proc->files[new_fd] = description;
        return new_fd;
}

i64_t proc_read(struct proc_table *table, struct proc *proc, int fd,
                void *dst, ksize_t count) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct proc_file *description = get_file(proc, fd);
        if (description == NULL) return PROC_EBADF;
        i64_t result = file_read(&description->file, dst, count);
        return result < 0 ? proc_fs_error((int)result) : result;
}

i64_t proc_write(struct proc_table *table, struct proc *proc, int fd,
                 const void *src, ksize_t count) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct proc_file *description = get_file(proc, fd);
        if (description == NULL) return PROC_EBADF;
        i64_t result = file_write(&description->file, src, count);
        return result < 0 ? proc_fs_error((int)result) : result;
}

int proc_chdir(struct proc_table *table, struct proc *proc, const char *path) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct inode *cwd;
        err = fs_lookup(table->filesystem, proc->cwd, path, &cwd);
        if (err != FS_OK) return proc_fs_error(err);
        if (cwd->type != DIR_NODE) { (void)inode_put(cwd); return PROC_ENOTDIR; }
        struct inode *old_cwd = proc->cwd;
        proc->cwd = cwd;
        (void)inode_put(old_cwd);
        return 0;
}

int proc_mkdir(struct proc_table *table, struct proc *proc, const char *path) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct inode *parent;
        char name[FS_NAME_MAX + 1];
        int trailing_slash;
        err = path_parent(table, proc, path, &parent, name, &trailing_slash);
        if (err != 0) return err;
        struct inode *inode;
        err = inode_create(parent, name, DIR_NODE, &inode);
        if (err == FS_OK) (void)inode_put(inode);
        (void)inode_put(parent);
        return proc_fs_error(err);
}

int proc_unlink(struct proc_table *table, struct proc *proc, const char *path) {
        int err = process_check(table, proc);
        if (err != 0) return err;
        struct inode *parent;
        char name[FS_NAME_MAX + 1];
        int trailing_slash;
        err = path_parent(table, proc, path, &parent, name, &trailing_slash);
        if (err != 0) return err;
        if (trailing_slash) {
                struct inode *target;
                err = dir_lookup(parent, name, &target);
                if (err == FS_OK) {
                        if (target->type != DIR_NODE) err = FS_ENOTDIR;
                        (void)inode_put(target);
                }
        } else err = FS_OK;
        if (err == FS_OK) err = dir_unlink(parent, name);
        (void)inode_put(parent);
        return proc_fs_error(err);
}
