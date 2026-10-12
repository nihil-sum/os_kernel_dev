#include "fs.h"
#include "kalloc.h"
#include "kstring.h"

struct inode_block {
	struct node node;
	u64_t index;
	u8_t data[FS_BLOCK_SIZE];
};

static int mounted(const struct fs *fs) {
	return fs != NULL && fs->root != NULL;
}

static int held(const struct inode *inode) {
	return inode != NULL && inode->valid && inode->ref != 0 && mounted(inode->fs);
}

static struct inode *new_inode(struct fs *fs, u64_t inum, enum inode_type type) {
	struct inode *inode = kalloc(sizeof(*inode));
	if (inode == NULL) return NULL;
	*inode = (struct inode){.fs = fs, .dev = fs->dev, .inum = inum,
		.type = type, .ref = 1, .valid = 1};
	node_init(&inode->node, inode);
	list_init(&inode->entries);
	list_init(&inode->blocks);
	return inode;
}

static void release_list(struct list_meta *list) {
	struct node *node;
	while ((node = list_pop_front(list)) != NULL)
		(void)kfree(node->data);
}

static void release_inode(struct inode *inode) {
	list_remove(&inode->fs->nodes, &inode->node);
	release_list(&inode->blocks);
	release_list(&inode->entries);
	inode->valid = 0;
	(void)kfree(inode);
}

int fs_init(struct fs *fs, u64_t dev) {
	if (fs == NULL) return FS_EINVAL;
	if (fs->root != NULL) return FS_EBUSY;
	/* Prepare everything before publishing or modifying the caller's state. */
	struct inode *root = kalloc(sizeof(*root));
	if (root == NULL) return FS_ENOMEM;
	*root = (struct inode){.fs = fs, .dev = dev, .inum = 1,
		.type = DIR_NODE, .nlink = 1, .valid = 1, .parent = root};
	node_init(&root->node, root);
	list_init(&root->entries);
	list_init(&root->blocks);
	*fs = (struct fs){.root = root, .dev = dev, .next_inum = 2};
	list_init(&fs->nodes);
	list_push_back(&fs->nodes, &root->node);
	return FS_OK;
}

int fs_destroy(struct fs *fs) {
	if (!mounted(fs)) return FS_EINVAL;
	for (struct node *node = fs->nodes.head; node != NULL; node = node->next) {
		struct inode *inode = node->data;
		if (inode->ref != 0) return FS_EBUSY;
	}
	/* Free records without following possibly already freed targets. */
	while (!list_empty(&fs->nodes)) release_inode(fs->nodes.head->data);
	*fs = (struct fs){0};
	return FS_OK;
}

int inode_alloc(struct fs *fs, enum inode_type type, struct inode **out) {
	if (!mounted(fs) || out == NULL || (type != DIR_NODE && type != FILE_NODE))
		return FS_EINVAL;
	if (fs->next_inum == 0) return FS_EOVERFLOW;
	struct inode *inode = new_inode(fs, fs->next_inum, type);
	if (inode == NULL) return FS_ENOMEM;
	fs->next_inum++;
	list_push_back(&fs->nodes, &inode->node);
	*out = inode;
	return FS_OK;
}

static int retain(struct inode *inode) {
	if (inode->ref == U32_MAX) return FS_EOVERFLOW;
	inode->ref++;
	return FS_OK;
}

int inode_get(struct fs *fs, u64_t inum, struct inode **out) {
	if (!mounted(fs) || out == NULL || inum == 0) return FS_EINVAL;
	for (struct node *node = fs->nodes.head; node != NULL; node = node->next) {
		struct inode *inode = node->data;
		if (inode->inum != inum) continue;
		int err = retain(inode);
		if (err == FS_OK) *out = inode;
		return err;
	}
	return FS_ENOENT;
}

int inode_ref(struct inode *inode) {
	return held(inode) ? retain(inode) : FS_EINVAL;
}

int inode_put(struct inode *inode) {
	if (!held(inode)) return FS_EINVAL;
	if (--inode->ref == 0 && inode->nlink == 0) release_inode(inode);
	return FS_OK;
}

static struct inode_block *find_block(struct inode *inode, u64_t index) {
	for (struct node *node = inode->blocks.head; node != NULL; node = node->next) {
		struct inode_block *block = node->data;
		if (block->index == index) return block;
	}
	return NULL;
}

i64_t inode_read(struct inode *inode, void *dst, u64_t offset, ksize_t count) {
	if (!held(inode) || (count != 0 && dst == NULL)) return FS_EINVAL;
	if (inode->type != FILE_NODE) return FS_EISDIR;
	if (offset >= inode->size) return 0;
	if (count > inode->size - offset) count = inode->size - offset;
	u8_t *bytes = dst;
	ksize_t done = 0;
	while (done < count) {
		ksize_t within = offset % FS_BLOCK_SIZE;
		ksize_t chunk = FS_BLOCK_SIZE - within;
		if (chunk > count - done) chunk = count - done;
		struct inode_block *block = find_block(inode, offset / FS_BLOCK_SIZE);
		if (block == NULL) memset(bytes + done, 0, chunk);
		else memcpy(bytes + done, block->data + within, chunk);
		done += chunk;
		offset += chunk;
	}
	return (i64_t)done;
}

i64_t inode_write(struct inode *inode, const void *src, u64_t offset, ksize_t count) {
	if (!held(inode) || (count != 0 && src == NULL)) return FS_EINVAL;
	if (inode->type != FILE_NODE) return FS_EISDIR;
	if (offset > FS_MAX_FILE_SIZE || count > FS_MAX_FILE_SIZE - offset)
		return FS_EFBIG;
	if (count == 0) return 0;
	u64_t end = offset + count;
	struct list_meta pending;
	list_init(&pending);
	/* Reserve every missing block before touching any live file bytes. */
	for (u64_t index = offset / FS_BLOCK_SIZE; index <= (end - 1) / FS_BLOCK_SIZE; index++) {
		if (find_block(inode, index) != NULL) continue;
		struct inode_block *block = kalloc(sizeof(*block));
		if (block == NULL) {
			release_list(&pending);
			return FS_ENOMEM;
		}
		block->index = index;
		memset(block->data, 0, sizeof(block->data));
		node_init(&block->node, block);
		list_push_back(&pending, &block->node);
	}
	struct node *node;
	while ((node = list_pop_front(&pending)) != NULL)
		list_push_back(&inode->blocks, node);
	const u8_t *bytes = src;
	ksize_t done = 0;
	while (done < count) {
		ksize_t within = offset % FS_BLOCK_SIZE;
		ksize_t chunk = FS_BLOCK_SIZE - within;
		if (chunk > count - done) chunk = count - done;
		struct inode_block *block = find_block(inode, offset / FS_BLOCK_SIZE);
		memcpy(block->data + within, bytes + done, chunk);
		done += chunk;
		offset += chunk;
	}
	if (end > inode->size) inode->size = end;
	return (i64_t)done;
}

int inode_truncate(struct inode *inode, u64_t size) {
	if (!held(inode)) return FS_EINVAL;
	if (inode->type != FILE_NODE) return FS_EISDIR;
	if (size > FS_MAX_FILE_SIZE) return FS_EFBIG;
	if (size < inode->size) {
		struct node *node = inode->blocks.head;
		while (node != NULL) {
			struct node *next = node->next;
			struct inode_block *block = node->data;
			if (block->index >= (size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE) {
				list_remove(&inode->blocks, node);
				(void)kfree(block);
			} else if (block->index == size / FS_BLOCK_SIZE) {
				ksize_t within = size % FS_BLOCK_SIZE;
				memset(block->data + within, 0, FS_BLOCK_SIZE - within);
			}
			node = next;
		}
	}
	inode->size = size;
	return FS_OK;
}

static int name_length(const char *name, ksize_t *length) {
	if (name == NULL || name[0] == '\0') return FS_EINVAL;
	for (ksize_t i = 0; i <= FS_NAME_MAX; i++) {
		if (name[i] == '\0') { *length = i; return FS_OK; }
		if (name[i] == '/') return FS_EINVAL;
	}
	return FS_ENAMETOOLONG;
}

static int special_name(const char *name, ksize_t length) {
	return (length == 1 && name[0] == '.') ||
	       (length == 2 && name[0] == '.' && name[1] == '.');
}

static struct dir_entry *find_entry(struct inode *dir, const char *name, ksize_t length) {
	for (struct node *node = dir->entries.head; node != NULL; node = node->next) {
		struct dir_entry *entry = node->data;
		ksize_t i = 0;
		while (i < length && entry->name[i] == name[i]) i++;
		if (i == length && entry->name[i] == '\0') return entry;
	}
	return NULL;
}

int dir_lookup(struct inode *dir, const char *name, struct inode **out) {
	if (!held(dir) || out == NULL) return FS_EINVAL;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	ksize_t length;
	int err = name_length(name, &length);
	if (err != FS_OK) return err;
	struct inode *target;
	if (special_name(name, length)) {
		target = length == 1 ? dir : dir->parent;
		if (target == NULL) return FS_ENOENT;
	} else {
		struct dir_entry *entry = find_entry(dir, name, length);
		if (entry == NULL) return FS_ENOENT;
		target = entry->target;
	}
	err = retain(target);
	if (err == FS_OK) *out = target;
	return err;
}

int dir_link(struct inode *dir, const char *name, struct inode *target) {
	if (!held(dir) || !held(target)) return FS_EINVAL;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	if (dir->fs != target->fs) return FS_EXDEV;
	ksize_t length;
	int err = name_length(name, &length);
	if (err != FS_OK) return err;
	if (special_name(name, length) || dir->nlink == 0) return FS_EPERM;
	if (find_entry(dir, name, length) != NULL) return FS_EEXIST;
	if (target->type == DIR_NODE &&
	    (target->nlink != 0 || target->parent != NULL || !list_empty(&target->entries)))
		return FS_EPERM;
	if (target->nlink == U32_MAX) return FS_EOVERFLOW;
	struct dir_entry *entry = kalloc(sizeof(*entry));
	if (entry == NULL) return FS_ENOMEM;
	memcpy(entry->name, name, length + 1);
	entry->target = target;
	node_init(&entry->node, entry);
	list_push_back(&dir->entries, &entry->node);
	dir->size++;
	target->nlink++;
	if (target->type == DIR_NODE) target->parent = dir;
	return FS_OK;
}

int dir_unlink(struct inode *dir, const char *name) {
	if (!held(dir)) return FS_EINVAL;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	ksize_t length;
	int err = name_length(name, &length);
	if (err != FS_OK) return err;
	if (special_name(name, length)) return FS_EPERM;
	struct dir_entry *entry = find_entry(dir, name, length);
	if (entry == NULL) return FS_ENOENT;
	struct inode *target = entry->target;
	if (target->type == DIR_NODE && !list_empty(&target->entries)) return FS_ENOTEMPTY;
	list_remove(&dir->entries, &entry->node);
	dir->size--;
	target->nlink--;
	if (target->type == DIR_NODE) target->parent = NULL;
	(void)kfree(entry);
	if (target->nlink == 0 && target->ref == 0) release_inode(target);
	return FS_OK;
}

int inode_create(struct inode *dir, const char *name, enum inode_type type,
		 struct inode **out) {
	if (!held(dir) || out == NULL) return FS_EINVAL;
	if (dir->type != DIR_NODE) return FS_ENOTDIR;
	ksize_t length;
	int err = name_length(name, &length);
	if (err != FS_OK) return err;
	if (special_name(name, length) || dir->nlink == 0) return FS_EPERM;
	if (find_entry(dir, name, length) != NULL) return FS_EEXIST;
	struct inode *inode;
	err = inode_alloc(dir->fs, type, &inode);
	if (err != FS_OK) return err;
	err = dir_link(dir, name, inode);
	if (err != FS_OK) { (void)inode_put(inode); return err; }
	*out = inode;
	return FS_OK;
}
