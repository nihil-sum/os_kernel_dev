#ifndef PROC_FS_H
#define PROC_FS_H

#include "proc.h"

enum proc_open_flag {
        PROC_O_READ = 1, PROC_O_WRITE = 2, PROC_O_CREATE = 4,
        PROC_O_EXCL = 8, PROC_O_TRUNC = 16
};

/* Kernel-pointer APIs; syscall entry points must copy/validate user memory.
 * Paths follow fs_lookup rules, relative to the process's retained cwd.
 * Only regular files can be opened. The lowest unused descriptor is returned,
 * including 0: standard input/output/error devices are not installed yet.
 * At least one access flag is required. EXCL requires CREATE; TRUNC requires
 * WRITE. Failed opens leave both the descriptor table and namespace unchanged.
 * All calls require the same serialization as the process/filesystem modules.
 */
int proc_open(struct proc_table *table, struct proc *proc, const char *path,
              unsigned int flags);
int proc_close(struct proc_table *table, struct proc *proc, int fd);
/* Duplicate a shared open description: dup and fork share a single cursor;
 * separate opens retain independent cursors. Descriptors survive exec.
 */
int proc_dup(struct proc_table *table, struct proc *proc, int fd);
i64_t proc_read(struct proc_table *table, struct proc *proc, int fd,
                void *dst, ksize_t count);
i64_t proc_write(struct proc_table *table, struct proc *proc, int fd,
                 const void *src, ksize_t count);
int proc_chdir(struct proc_table *table, struct proc *proc, const char *path);
int proc_mkdir(struct proc_table *table, struct proc *proc, const char *path);
/* Removes a regular-file name or an empty directory, as dir_unlink does.
 * A trailing slash requires a directory; root, '.' and '..' are protected.
 */
int proc_unlink(struct proc_table *table, struct proc *proc, const char *path);

/* Internal lifecycle hooks. start retains the table root (or does nothing
 * without a filesystem). fork replaces the child's initial cwd and copies
 * descriptor references; failure preserves its original state. release is
 * idempotent and drops every retained resource, normally at process exit.
 */
int proc_fs_start(struct proc_table *table, struct proc *proc);
int proc_fs_fork(struct proc *child, const struct proc *parent);
void proc_fs_release(struct proc *proc);
int proc_fs_error(int error);

#endif
