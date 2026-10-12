#include "shell_fs.h"
#include "console.h"
#include "proc_fs.h"

static struct {
        struct fs *fs;
        struct inode *cwd;
} files;

static int equal(const char *a, const char *b) {
        while (*a != '\0' && *a == *b) { a++; b++; }
        return *a == *b;
}

int shell_fs_init(struct fs *fs) {
        if (files.cwd != NULL) return PROC_EBUSY;
        if (fs == NULL || fs->root == NULL) return PROC_EINVAL;
        struct inode *root;
        int err = inode_get(fs, fs->root->inum, &root);
        if (err != FS_OK) return proc_fs_error(err);
        files.fs = fs;
        files.cwd = root;
        return 0;
}

void shell_fs_destroy(void) {
        if (files.cwd != NULL) (void)inode_put(files.cwd);
        files.cwd = NULL;
        files.fs = NULL;
}

int shell_path_parent(struct fs *fs, struct inode *cwd, const char *path,
                      struct inode **parent, char name[FS_NAME_MAX + 1],
                      int *trailing) {
        if (path == NULL || path[0] == '\0') return FS_EINVAL;
        ksize_t length = 0;
        while (length <= FS_PATH_MAX && path[length] != '\0') length++;
        if (length > FS_PATH_MAX) return FS_ENAMETOOLONG;
        ksize_t end = length;
        while (end != 0 && path[end - 1] == '/') end--;
        if (end == 0) return FS_EPERM;
        ksize_t begin = end;
        while (begin != 0 && path[begin - 1] != '/') begin--;
        ksize_t count = end - begin;
        if (count > FS_NAME_MAX) return FS_ENAMETOOLONG;
        for (ksize_t i = 0; i < count; i++) name[i] = path[begin + i];
        name[count] = '\0';
        if (equal(name, ".") || equal(name, "..")) return FS_EPERM;
        *trailing = end != length;
        if (begin == 0) {
                int err = inode_ref(cwd);
                if (err == FS_OK) *parent = cwd;
                return err;
        }
        char prefix[FS_PATH_MAX + 1];
        for (ksize_t i = 0; i < begin; i++) prefix[i] = path[i];
        prefix[begin] = '\0';
        return fs_lookup(fs, cwd, prefix, parent);
}

static int pwd(void) {
        char path[FS_PATH_MAX + 1];
        int err = fs_getpath(files.fs, files.cwd, path, sizeof(path));
        if (err == FS_OK) { console_puts(path); console_putc('\n'); }
        return proc_fs_error(err);
}

static int cd(const char *path) {
        struct inode *next;
        int err = fs_lookup(files.fs, files.cwd, path, &next);
        if (err != FS_OK) return proc_fs_error(err);
        if (next->type != DIR_NODE) {
                (void)inode_put(next);
                return PROC_ENOTDIR;
        }
        struct inode *old = files.cwd;
        files.cwd = next;
        (void)inode_put(old);
        return 0;
}

static int create(const char *path, enum inode_type type) {
        /* touch preserves contents of an existing regular file. This RAM
         * filesystem has no timestamps to update.
         */
        struct inode *inode;
        int err;
        if (type == FILE_NODE) {
                err = fs_lookup(files.fs, files.cwd, path, &inode);
                if (err == FS_OK) {
                        int result = inode->type == FILE_NODE ? 0 : PROC_EISDIR;
                        (void)inode_put(inode);
                        return result;
                }
                if (err != FS_ENOENT) return proc_fs_error(err);
        }
        struct inode *parent;
        char name[FS_NAME_MAX + 1];
        int trailing;
        err = shell_path_parent(files.fs, files.cwd, path, &parent, name, &trailing);
        if (err != FS_OK) return proc_fs_error(err);
        if (type == FILE_NODE && trailing) err = FS_ENOTDIR;
        else err = inode_create(parent, name, type, &inode);
        if (err == FS_OK) (void)inode_put(inode);
        (void)inode_put(parent);
        return proc_fs_error(err);
}

static void listing(enum inode_type type, u64_t size, u64_t inum, const char *name) {
        console_putc(type == DIR_NODE ? 'd' : 'f'); console_putc(' ');
        console_u64(size); console_putc(' ');
        console_u64(inum); console_putc(' ');
        console_puts(name);
        if (type == DIR_NODE) console_putc('/');
        console_putc('\n');
}

static int ls(const char *path) {
        struct inode *inode;
        int err = fs_lookup(files.fs, files.cwd, path, &inode);
        if (err != FS_OK) return proc_fs_error(err);
        console_puts("TYPE SIZE INODE NAME\n");
        if (inode->type == FILE_NODE) listing(inode->type, inode->size, inode->inum, path);
        else {
                struct fs_dirent entry;
                u64_t index = 0;
                while ((err = dir_read(inode, index, &entry)) == 1) {
                        listing(entry.type, entry.size, entry.inum, entry.name);
                        index++;
                }
                if (index == 0 && err == 0) console_puts("(empty)\n");
        }
        (void)inode_put(inode);
        return err < 0 ? proc_fs_error(err) : 0;
}

static int cat(const char *path) {
        struct inode *inode;
        int err = fs_lookup(files.fs, files.cwd, path, &inode);
        if (err != FS_OK) return proc_fs_error(err);
        char buffer[256];
        u64_t offset = 0;
        char last = '\n';
        i64_t count;
        while ((count = inode_read(inode, buffer, offset, sizeof(buffer))) > 0) {
                for (i64_t i = 0; i < count; i++) {
                        unsigned char ch = (unsigned char)buffer[i];
                        last = (char)ch;
                        console_putc(ch == '\n' || ch == '\t' || (ch >= 32 && ch <= 126) ?
                                     (char)ch : '?');
                }
                offset += (u64_t)count;
        }
        if (last != '\n') console_putc('\n');
        (void)inode_put(inode);
        return count < 0 ? proc_fs_error((int)count) : 0;
}

static int rm(const char *path) {
        struct inode *parent;
        char name[FS_NAME_MAX + 1];
        int trailing;
        int err = shell_path_parent(files.fs, files.cwd, path, &parent, name, &trailing);
        if (err != FS_OK) return proc_fs_error(err);
        struct inode *target;
        err = dir_lookup(parent, name, &target);
        if (err == FS_OK) {
                if (target == files.cwd) err = FS_EBUSY;
                else if (trailing && target->type != DIR_NODE) err = FS_ENOTDIR;
                (void)inode_put(target);
        }
        if (err == FS_OK) err = dir_unlink(parent, name);
        (void)inode_put(parent);
        return proc_fs_error(err);
}

int shell_fs_command(ksize_t argc, char **argv) {
        if (files.fs == NULL || files.cwd == NULL) return PROC_ENOSYS;
        if (equal(argv[0], "pwd")) return pwd();
        if (equal(argv[0], "cd")) return cd(argc == 1 ? "/" : argv[1]);
        if (equal(argv[0], "mkdir")) return create(argv[1], DIR_NODE);
        if (equal(argv[0], "touch")) return create(argv[1], FILE_NODE);
        if (equal(argv[0], "ls")) return ls(argc == 1 ? "." : argv[1]);
        if (equal(argv[0], "cat")) return cat(argv[1]);
        if (equal(argv[0], "rm")) return rm(argv[1]);
        if (equal(argv[0], "editor") || equal(argv[0], "edit"))
                return shell_editor(files.fs, files.cwd, argv[1]);
        return PROC_EINVAL;
}
