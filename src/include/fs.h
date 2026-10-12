#ifndef FS_H
#define FS_H

#include "types.h"
#include "list.h"

#define FS_NAME_MAX 63u
#define FS_PATH_MAX 1024u
#define FS_BLOCK_SIZE 512u
#define FS_MAX_FILE_SIZE (1024u * 1024u)
#define STDIN_FN 0
#define STDOUT_FN 1
#define STDERR_FN 2

enum inode_type { DIR_NODE = 0, FILE_NODE = 1 };
enum fs_error {
	FS_OK = 0, FS_EINVAL = -1, FS_ENOMEM = -2, FS_ENOENT = -3,
	FS_EEXIST = -4, FS_ENOTDIR = -5, FS_EISDIR = -6, FS_ENOTEMPTY = -7,
	FS_EBUSY = -8, FS_ENAMETOOLONG = -9, FS_EFBIG = -10,
	FS_EXDEV = -11, FS_EPERM = -12, FS_EOVERFLOW = -13
};

struct fs;
struct inode {
	struct node node;            /* Member of fs.nodes. */
	struct fs *fs;
	u64_t dev, inum;
	enum inode_type type;
	u64_t size;                  /* File bytes, or directory entry count. */
	u32_t ref;                   /* Held kernel references/open handles. */
	u32_t nlink;                 /* Explicit names; root has one mount link. */
	int valid;
	struct inode *parent;        /* Directory only; cleared on unlink. */
	struct list_meta entries;    /* Directory entries. */
	struct list_meta blocks;     /* Private file data blocks. */
};

struct dir_entry {
	struct node node;
	char name[FS_NAME_MAX + 1];
	struct inode *target;        /* Owns a link, not a ref. */
};

/* Start with {0}. Storage must stay at a stable address while mounted.
 * All operations, including kalloc/buddy access, require serialization.
 * This is a volatile RAM filesystem, with no disk or process-fd dependency.
 */
struct fs {
	struct list_meta nodes;
	struct inode *root;
	u64_t dev;
	u64_t next_inum;
};

int fs_init(struct fs *fs, u64_t dev);
int make_filesystem(struct fs *fs); /* fs_init(fs, 0); no disk formatting. */
/* Refuse any held inode/file references; otherwise release the whole tree. */
int fs_destroy(struct fs *fs);

/* Successful alloc/get/lookup/create calls return one reference in *out;
 * outputs are untouched on errors. Use inode_put to release each reference.
 * alloc creates an unnamed inode (nlink == 0); get uses filesystem-local inum.
 * Except get, APIs taking inodes require live pointers with a held reference.
 * Never use a pointer after its last reference is released.
 */
int inode_alloc(struct fs *fs, enum inode_type type, struct inode **out);
int inode_get(struct fs *fs, u64_t inum, struct inode **out);
int inode_ref(struct inode *inode);
int inode_put(struct inode *inode);

/* Byte count or negative fs_error. Kernel buffers must be valid, disjoint
 * from filesystem-owned storage. NULL buffers are allowed for zero length.
 * Holes read as zero. Writes either complete or leave contents/size unchanged.
 * Offsets are explicit: the inode never owns an open-file cursor.
 */
i64_t inode_read(struct inode *inode, void *dst, u64_t offset, ksize_t count);
i64_t inode_write(struct inode *inode, const void *src, u64_t offset, ksize_t count);
int inode_truncate(struct inode *inode, u64_t size);

/* Names have 1..FS_NAME_MAX bytes and contain no '/'. Lookup recognizes '.'
 * and '..'; mutation rejects them. Linking a directory is allowed only once,
 * while empty and unattached. Only attached directories accept new entries.
 * Unlink rejects nonempty directories; unlinked files survive while ref > 0.
 */
int dir_lookup(struct inode *dir, const char *name, struct inode **out);
int dir_link(struct inode *dir, const char *name, struct inode *target);
int dir_unlink(struct inode *dir, const char *name);
int inode_create(struct inode *dir, const char *name, enum inode_type type,
		 struct inode **out);

/* Snapshot an explicit directory entry, in insertion order. No inode reference
 * is transferred. Returns 1 for an entry, 0 at EOF, or a negative fs_error;
 * out is unchanged at EOF/error. Indices are not stable across mutations.
 */
struct fs_dirent {
	char name[FS_NAME_MAX + 1];
	u64_t inum, size;
	enum inode_type type;
};
int dir_read(struct inode *dir, u64_t index, struct fs_dirent *out);

/* Absolute paths start at fs.root; relative paths require a held start inode
 * in this filesystem. Repeated '/' and '.'/'..' are supported. Empty paths
 * fail; trailing '/' requires a directory. Maximum path length is 1024 bytes.
 */
int fs_lookup(struct fs *fs, struct inode *start, const char *path,
	      struct inode **out);
/* Canonical absolute path for a held directory. No allocation or recursion.
 * size includes NUL; a small buffer/overlong path returns FS_ENAMETOOLONG.
 * Detached directories return FS_ENOENT. Buffer is unspecified on failure.
 */
int fs_getpath(struct fs *fs, struct inode *dir, char *buffer, ksize_t size);

enum file_mode { FILE_READ = 1, FILE_WRITE = 2 };
/* Caller-owned, initially {0}. Do not copy a live handle. Each open retains
 * an inode reference and has an independent offset; no fd table is implied.
 */
struct file {
	struct inode *inode;
	u64_t offset;
	unsigned int mode;
};
int file_open(struct file *file, struct inode *inode, unsigned int mode);
int file_close(struct file *file);
i64_t file_read(struct file *file, void *dst, ksize_t count);
i64_t file_write(struct file *file, const void *src, ksize_t count);

#endif
