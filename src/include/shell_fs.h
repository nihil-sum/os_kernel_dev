#ifndef SHELL_FS_H
#define SHELL_FS_H

#include "fs.h"

/* Shell-owned cwd, independent of process slots and descriptor tables.
 * Call once on an initialized filesystem; all calls require serialization.
 * Command/editor return values are proc_error values for the shell reporter.
 */
int shell_fs_init(struct fs *fs);
void shell_fs_destroy(void);
int shell_fs_command(ksize_t argc, char **argv);
int shell_editor(struct fs *fs, struct inode *cwd, const char *path);

/* Shared mutation helper for shell commands and editor. Returns fs_error.
 * Success transfers one parent reference; caller releases it with inode_put.
 * Root, '.' and '..' cannot be mutation targets. trailing is true for '/'.
 */
int shell_path_parent(struct fs *fs, struct inode *cwd, const char *path,
                      struct inode **parent, char name[FS_NAME_MAX + 1],
                      int *trailing);

#endif
