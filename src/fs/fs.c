#include "fs.h"
#include "kstring.h"

static int held(const struct inode *inode) {
	return inode != NULL && inode->valid && inode->ref != 0 &&
	       inode->fs != NULL && inode->fs->root != NULL;
}

int dir_read(struct inode *dir, u64_t index, struct fs_dirent *out) {
	if (!held(dir) || out == NULL) return FS_EINVAL;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	if (index >= dir->size) return 0;
	struct node *node = dir->entries.head;
	while (index != 0 && node != NULL) { node = node->next; index--; }
	if (node == NULL) return 0;
	const struct dir_entry *entry = node->data;
	struct fs_dirent result = {.inum = entry->target->inum,
		.size = entry->target->size, .type = entry->target->type};
	ksize_t n = 0;
	while (entry->name[n] != '\0') n++;
	memcpy(result.name, entry->name, n + 1);
	*out = result;
	return 1;
}

int fs_getpath(struct fs *fs, struct inode *dir, char *buffer, ksize_t size) {
	if (fs == NULL || fs->root == NULL || !held(dir) || buffer == NULL || size == 0)
		return FS_EINVAL;
	if (dir->fs != fs) return FS_EXDEV;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	if (size > FS_PATH_MAX + 1) size = FS_PATH_MAX + 1;
	ksize_t start = size - 1;
	buffer[start] = '\0';
	for (struct inode *current = dir; current != fs->root; current = current->parent) {
		struct inode *parent = current->parent;
		if (parent == NULL) return FS_ENOENT;
		const struct dir_entry *found = NULL;
		for (struct node *node = parent->entries.head; node != NULL; node = node->next) {
			const struct dir_entry *entry = node->data;
			if (entry->target == current) { found = entry; break; }
		}
		if (found == NULL) return FS_ENOENT;
		ksize_t length = 0;
		while (found->name[length] != '\0') length++;
		if (length + 1 > start) return FS_ENAMETOOLONG;
		start -= length;
		memcpy(buffer + start, found->name, length);
		buffer[--start] = '/';
	}
	if (dir == fs->root) {
		if (start == 0) return FS_ENAMETOOLONG;
		buffer[--start] = '/';
	}
	/* Forward copying is safe when moving toward the beginning of the buffer. */
	for (ksize_t i = 0; i < size - start; i++) buffer[i] = buffer[start + i];
	return FS_OK;
}

int fs_lookup(struct fs *fs, struct inode *start, const char *path,
	      struct inode **out) {
	if (fs == NULL || fs->root == NULL || path == NULL || out == NULL || path[0] == '\0')
		return FS_EINVAL;
	ksize_t length = 0;
	while (length <= FS_PATH_MAX && path[length] != '\0') length++;
	if (length > FS_PATH_MAX) return FS_ENAMETOOLONG;
	struct inode *current;
	int err;
	if (path[0] == '/') {
		err = inode_get(fs, fs->root->inum, &current);
	} else {
		if (!held(start)) return FS_EINVAL;
		if (start->fs != fs) return FS_EXDEV;
		err = inode_ref(start);
		current = start;
	}
	if (err != FS_OK) return err;
	ksize_t pos = 0;
	while (pos < length) {
		/* Even a trailing slash after a file must fail. */
		if (current->type != DIR_NODE) { err = FS_ENOTDIR; goto fail; }
		while (pos < length && path[pos] == '/') pos++;
		if (pos == length) break;
		char name[FS_NAME_MAX + 1];
		ksize_t count = 0;
		while (pos < length && path[pos] != '/') {
			if (count == FS_NAME_MAX) { err = FS_ENAMETOOLONG; goto fail; }
			name[count++] = path[pos++];
		}
		name[count] = '\0';
		struct inode *next;
		err = dir_lookup(current, name, &next);
		if (err != FS_OK) goto fail;
		(void)inode_put(current);
		current = next;
	}
	*out = current;
	return FS_OK;
fail:
	(void)inode_put(current);
	return err;
}

int file_open(struct file *file, struct inode *inode, unsigned int mode) {
	if (file == NULL || !held(inode) || mode == 0 ||
	    (mode & ~(FILE_READ | FILE_WRITE)) != 0) return FS_EINVAL;
	if (file->inode != NULL) return FS_EBUSY;
	if (inode->type != FILE_NODE) return FS_EISDIR;
	int err = inode_ref(inode);
	if (err != FS_OK) return err;
	*file = (struct file){.inode = inode, .mode = mode};
	return FS_OK;
}

int file_close(struct file *file) {
	if (file == NULL || !held(file->inode)) return FS_EINVAL;
	int err = inode_put(file->inode);
	if (err == FS_OK) *file = (struct file){0};
	return err;
}

i64_t file_read(struct file *file, void *dst, ksize_t count) {
	if (file == NULL || !held(file->inode)) return FS_EINVAL;
	if ((file->mode & FILE_READ) == 0) return FS_EPERM;
	i64_t result = inode_read(file->inode, dst, file->offset, count);
	if (result > 0) file->offset += (u64_t)result;
	return result;
}

i64_t file_write(struct file *file, const void *src, ksize_t count) {
	if (file == NULL || !held(file->inode)) return FS_EINVAL;
	if ((file->mode & FILE_WRITE) == 0) return FS_EPERM;
	i64_t result = inode_write(file->inode, src, file->offset, count);
	if (result > 0) file->offset += (u64_t)result;
	return result;
}
